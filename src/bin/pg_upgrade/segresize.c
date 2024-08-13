/*
 *	segresize.c
 *
 *	SLRU segment resize utility
 *
 *	Copyright (c) 2024, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/segresize.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"
#include "access/multixact.h"

/* See slru.h */
#define SLRU_PAGES_PER_SEGMENT		32

/*
 * Some kind of iterator associated with a particular SLRU segment.  The idea is
 * to specify the segment and page number and then move through the pages.
 */
typedef struct SlruSegState
{
	char	   *dir;
	char	   *fn;
	FILE	   *file;
	int64		segno;
	uint64		pageno;
	bool		leading_gap;
} SlruSegState;

/*
 * Mirrors the SlruFileName from slru.c
 */
static inline char *
SlruFileName(SlruSegState *state)
{
	Assert(state->segno >= 0 && state->segno <= INT64CONST(0xFFFFFF));
	return psprintf("%s/%04X", state->dir, (unsigned int) state->segno);
}

/*
 * Create new SLRU segment file.
 */
static void
create_segment(SlruSegState *state)
{
	Assert(state->fn == NULL);
	Assert(state->file == NULL);

	state->fn = SlruFileName(state);
	state->file = fopen(state->fn, "wb");
	if (!state->file)
		pg_fatal("could not create file \"%s\": %m", state->fn);
}

/*
 * Open existing SLRU segment file.
 */
static void
open_segment(SlruSegState *state)
{
	Assert(state->fn == NULL);
	Assert(state->file == NULL);

	state->fn = SlruFileName(state);
	state->file = fopen(state->fn, "rb");
	if (!state->file)
		pg_fatal("could not open file \"%s\": %m", state->fn);
}

/*
 * Close SLRU segment file.
 */
static void
close_segment(SlruSegState *state)
{
	if (state->file)
	{
		fclose(state->file);
		state->file = NULL;
	}

	if (state->fn)
	{
		pfree(state->fn);
		state->fn = NULL;
	}
}

/*
 * Read next page from the old 32-bit offset segment file.
 */
static int
read_old_segment_page(SlruSegState *state, void *buf, bool *empty)
{
	int		len;

	/* Open next segment file, if needed. */
	if (!state->fn)
	{
		if (!state->segno)
			state->leading_gap = true;

		open_segment(state);

		/* Set position to the needed page. */
		if (state->pageno > 0 &&
			fseek(state->file, state->pageno * BLCKSZ, SEEK_SET))
		{
			close_segment(state);
		}
	}

	if (state->file)
	{
		/* Segment file do exists, read page from it. */
		state->leading_gap = false;

		len = fread(buf, sizeof(char), BLCKSZ, state->file);

		/* Are we done or was there an error? */
		if (len <= 0)
		{
			if (ferror(state->file))
				pg_fatal("error reading file \"%s\": %m", state->fn);

			if (feof(state->file))
			{
				*empty = true;
				len = -1;

				close_segment(state);
			}
		}
		else
			*empty = false;
	}
	else if (!state->leading_gap)
	{
		/* We reached the last segment. */
		len = -1;
		*empty = true;
	}
	else
	{
		/* Skip few first segments if they were frozen and removed. */
		len = BLCKSZ;
		*empty = true;
	}

	if (++state->pageno >= SLRU_PAGES_PER_SEGMENT)
	{
		/* Start a new segment. */
		state->segno++;
		state->pageno = 0;

		close_segment(state);
	}

	return len;
}

/*
 * Write next page to the new 64-bit offset segment file.
 */
static void
write_new_segment_page(SlruSegState *state, void *buf)
{
	/*
	 * Create a new segment file if we still didn't.  Creation is
	 * postponed until the first non-empty page is found.  This helps
	 * not to create completely empty segments.
	 */
	if (!state->file)
	{
		create_segment(state);

		/* Write zeroes to the previously skipped prefix. */
		if (state->pageno > 0)
		{
			char		zerobuf[BLCKSZ] = {0};

			for (int64 i = 0; i < state->pageno; i++)
			{
				if (fwrite(zerobuf, sizeof(char), BLCKSZ, state->file) != BLCKSZ)
					pg_fatal("could not write file \"%s\": %m", state->fn);
			}
		}
	}

	/* Write page to the new segment (if it was created). */
	if (state->file)
	{
		if (fwrite(buf, sizeof(char), BLCKSZ, state->file) != BLCKSZ)
			pg_fatal("could not write file \"%s\": %m", state->fn);
	}

	/*
	 * Did we reach the maximum page number?  Then close segment file
	 * and create a new one on the next iteration.
	 */
	if (++state->pageno >= SLRU_PAGES_PER_SEGMENT)
	{
		/* Start a new segment. */
		state->segno++;
		state->pageno = 0;

		close_segment(state);
	}
}

typedef uint32 MultiXactOffsetOld;

#define MaxMultiXactOffsetOld	((MultiXactOffsetOld) 0xFFFFFFFF)

#define MULTIXACT_OFFSETS_PER_PAGE_OLD (BLCKSZ / sizeof(MultiXactOffsetOld))
#define MULTIXACT_OFFSETS_PER_PAGE_NEW (BLCKSZ / sizeof(MultiXactOffset))

/*
 * Convert pg_multixact/offsets segments and return oldest multi offset.
 */
MultiXactOffset
convert_multixact_offsets(void)
{
	SlruSegState		oldseg = {0},
						newseg = {0};
	MultiXactOffsetOld	oldbuf[MULTIXACT_OFFSETS_PER_PAGE_OLD] = {0};
	MultiXactOffset		newbuf[MULTIXACT_OFFSETS_PER_PAGE_NEW] = {0},
						oldest_offset = 0;
	uint64				oldest_multi = old_cluster.controldata.chkpnt_oldstMulti,
						next_multi = old_cluster.controldata.chkpnt_nxtmulti,
						multi,
						old_entry,
						new_entry;
	bool				oldest_offset_known = false;

	oldseg.dir = psprintf("%s/pg_multixact/offsets", old_cluster.pgdata);
	newseg.dir = psprintf("%s/pg_multixact/offsets", new_cluster.pgdata);

	old_entry = oldest_multi % MULTIXACT_OFFSETS_PER_PAGE_OLD;
	oldseg.pageno = oldest_multi / MULTIXACT_OFFSETS_PER_PAGE_OLD;
	oldseg.segno = oldseg.pageno / SLRU_PAGES_PER_SEGMENT;
	oldseg.pageno %= SLRU_PAGES_PER_SEGMENT;

	new_entry = oldest_multi % MULTIXACT_OFFSETS_PER_PAGE_NEW;
	newseg.pageno = oldest_multi / MULTIXACT_OFFSETS_PER_PAGE_NEW;
	newseg.segno = newseg.pageno / SLRU_PAGES_PER_SEGMENT;
	newseg.pageno %= SLRU_PAGES_PER_SEGMENT;

	if (next_multi < oldest_multi)
		next_multi += (uint64) 1 << 32;	/* wraparound */

	/* Copy multi offsets reading only needed segment pages */
	for (multi = oldest_multi; multi < next_multi; old_entry = 0)
	{
		int		oldlen;
		bool	empty;

		/* Handle possible segment wraparound */
#define OLD_OFFSET_SEGNO_MAX	\
	(MaxMultiXactId / MULTIXACT_OFFSETS_PER_PAGE_OLD / SLRU_PAGES_PER_SEGMENT)
		if (oldseg.segno > OLD_OFFSET_SEGNO_MAX)
		{
			oldseg.segno = 0;
			oldseg.pageno = 0;
		}

		oldlen = read_old_segment_page(&oldseg, oldbuf, &empty);
		if (empty || oldlen != BLCKSZ)
			pg_fatal("cannot read page %" PRIu64 " from file \"%s\": %m",
					 oldseg.pageno, oldseg.fn);

		/* Save oldest multi offset */
		if (!oldest_offset_known)
		{
			oldest_offset = oldbuf[old_entry];
			oldest_offset_known = true;
		}

		/* Skip wrapped-around invalid MultiXactIds */
		if (multi == (uint64) 1 << 32)
		{
			Assert(oldseg.segno == 0);
			Assert(oldseg.pageno == 1);
			Assert(old_entry == 0);
			Assert(new_entry == 0);

			multi += FirstMultiXactId;
			old_entry = FirstMultiXactId;
			new_entry = FirstMultiXactId;
		}

		/* Copy entries to the new page */
		for (; multi < next_multi && old_entry < MULTIXACT_OFFSETS_PER_PAGE_OLD;
			 multi++, old_entry++)
		{
			MultiXactOffset offset = oldbuf[old_entry];

			/* Handle possible offset wraparound (1 becomes 2^32) */
			if (offset < oldest_offset)
				offset += ((uint64) 1 << 32) - 1;

			/* Subtract oldest_offset, so new offsets will start from 1 */
			newbuf[new_entry++] = offset - oldest_offset + 1;

			if (new_entry >= MULTIXACT_OFFSETS_PER_PAGE_NEW)
			{
				/* Handle possible segment wraparound */
#define NEW_OFFSET_SEGNO_MAX	\
	(MaxMultiXactId / MULTIXACT_OFFSETS_PER_PAGE_NEW / SLRU_PAGES_PER_SEGMENT)
				if (newseg.segno > NEW_OFFSET_SEGNO_MAX)
				{
					newseg.segno = 0;
					newseg.pageno = 0;
				}

				/* Write new page */
				write_new_segment_page(&newseg, newbuf);
				new_entry = 0;
			}
		}
	}

	/* Write the last incomplete page */
	if (new_entry > 0 || oldest_multi == next_multi)
	{
		memset(&newbuf[new_entry], 0,
			   sizeof(newbuf[0]) * (MULTIXACT_OFFSETS_PER_PAGE_NEW - new_entry));
		write_new_segment_page(&newseg, newbuf);
	}

	/* Use next_offset as oldest_offset, if oldest_multi == next_multi */
	if (!oldest_offset_known)
	{
		Assert(oldest_multi == next_multi);
		oldest_offset = (MultiXactOffset) old_cluster.controldata.chkpnt_nxtmxoff;
	}

	/* Release resources */
	close_segment(&oldseg);
	close_segment(&newseg);

	pfree(oldseg.dir);
	pfree(newseg.dir);

	return oldest_offset;
}

#define MXACT_MEMBERS_FLAG_BYTES			1

#define MULTIXACT_MEMBERS_PER_GROUP			4
#define MULTIXACT_MEMBERGROUP_SIZE			\
	(MULTIXACT_MEMBERS_PER_GROUP * (sizeof(TransactionId) + MXACT_MEMBERS_FLAG_BYTES))
#define MULTIXACT_MEMBERGROUPS_PER_PAGE		\
	(BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)

#define MULTIXACT_MEMBERS_PER_PAGE				\
	(MULTIXACT_MEMBERS_PER_GROUP * MULTIXACT_MEMBERGROUPS_PER_PAGE)
#define MULTIXACT_MEMBER_FLAG_BYTES_PER_GROUP	\
	(MXACT_MEMBERS_FLAG_BYTES * MULTIXACT_MEMBERS_PER_GROUP)

typedef struct MultiXactMembersCtx
{
	SlruSegState	seg;
	char			buf[BLCKSZ];
	int				group;
	int				member;
	char		   *flag;
	TransactionId  *xid;
} MultiXactMembersCtx;

static void
MultiXactMembersCtxInit(MultiXactMembersCtx *ctx)
{
	ctx->seg.dir = psprintf("%s/pg_multixact/members", new_cluster.pgdata);

	ctx->group = 0;
	ctx->member = 1;		/* skip invalid zero offset */

	ctx->flag = (char *) ctx->buf + ctx->group * MULTIXACT_MEMBERGROUP_SIZE;
	ctx->xid = (TransactionId *)(ctx->flag + MXACT_MEMBERS_FLAG_BYTES * MULTIXACT_MEMBERS_PER_GROUP);

	ctx->flag += ctx->member;
	ctx->xid += ctx->member;
}

static void
MultiXactMembersCtxAdd(MultiXactMembersCtx *ctx, char flag, TransactionId xid)
{
	/* Copy member's xid and flags to the new page */
	*ctx->flag++ = flag;
	*ctx->xid++ = xid;

	if (++ctx->member < MULTIXACT_MEMBERS_PER_GROUP)
		return;

	/* Start next member group */
	ctx->member = 0;

	if (++ctx->group >= MULTIXACT_MEMBERGROUPS_PER_PAGE)
	{
		/* Write current page and start new */
		write_new_segment_page(&ctx->seg, ctx->buf);

		ctx->group = 0;
		memset(ctx->buf, 0, BLCKSZ);
	}

	ctx->flag = (char *) ctx->buf + ctx->group * MULTIXACT_MEMBERGROUP_SIZE;
	ctx->xid = (TransactionId *)(ctx->flag + MXACT_MEMBERS_FLAG_BYTES * MULTIXACT_MEMBERS_PER_GROUP);
}

static void
MultiXactMembersCtxFinit(MultiXactMembersCtx *ctx)
{
	if (ctx->flag > (char *) ctx->buf)
		write_new_segment_page(&ctx->seg, ctx->buf);

	close_segment(&ctx->seg);

	pfree(ctx->seg.dir);
}

/*
 * Convert pg_multixact/members segments, offsets will start from 1.
 *
 */
void
convert_multixact_members(MultiXactOffset oldest_offset)
{
	MultiXactOffset			next_offset,
							offset;
	SlruSegState			oldseg = {0};
	char					oldbuf[BLCKSZ] = {0};
	int						oldidx;
	MultiXactMembersCtx		newctx = {0};

	oldseg.dir = psprintf("%s/pg_multixact/members", old_cluster.pgdata);

	next_offset = (MultiXactOffset) old_cluster.controldata.chkpnt_nxtmxoff;
	if (next_offset < oldest_offset)
		next_offset += ((uint64) 1 << 32) - 1;

	/* Initialize the old starting position */
	oldseg.pageno = oldest_offset / MULTIXACT_MEMBERS_PER_PAGE;
	oldseg.segno = oldseg.pageno / SLRU_PAGES_PER_SEGMENT;
	oldseg.pageno %= SLRU_PAGES_PER_SEGMENT;

	/* Initialize new starting position */
	MultiXactMembersCtxInit(&newctx);

	/* Iterate through the original directory */
	oldidx = oldest_offset % MULTIXACT_MEMBERS_PER_PAGE;
	for (offset = oldest_offset; offset < next_offset;)
	{
		bool	empty;
		int		oldlen;
		int		ngroups;
		int		oldgroup;
		int		oldmember;

		oldlen = read_old_segment_page(&oldseg, oldbuf, &empty);
		if (empty || oldlen != BLCKSZ)
			pg_fatal("cannot read page %" PRIu64 " from file \"%s\": %m",
					 oldseg.pageno, oldseg.fn);

		/* Iterate through the old member groups */
		ngroups = oldlen / MULTIXACT_MEMBERGROUP_SIZE;
		oldmember = oldidx % MULTIXACT_MEMBERS_PER_GROUP;
		oldgroup = oldidx / MULTIXACT_MEMBERS_PER_GROUP;
		while (oldgroup < ngroups && offset < next_offset)
		{
			char		   *oldflag;
			TransactionId  *oldxid;
			int				i;

			oldflag = (char *) oldbuf + oldgroup * MULTIXACT_MEMBERGROUP_SIZE;
			oldxid = (TransactionId *)(oldflag + MULTIXACT_MEMBER_FLAG_BYTES_PER_GROUP);

			oldxid += oldmember;
			oldflag += oldmember;

			/* Iterate through the old members */
			for (i = oldmember;
				 i < MULTIXACT_MEMBERS_PER_GROUP && offset < next_offset;
				 i++)
			{
				MultiXactMembersCtxAdd(&newctx, *oldflag++, *oldxid++);

				if (++offset == (uint64) 1 << 32)
				{
					Assert(i == MaxMultiXactOffsetOld % MULTIXACT_MEMBERS_PER_GROUP);
					goto wraparound;
				}
			}

			oldgroup++;
			oldmember = 0;
		}

		oldidx = 0;

		continue;

wraparound:
#define SEGNO_MAX	MaxMultiXactOffsetOld / MULTIXACT_MEMBERS_PER_PAGE / SLRU_PAGES_PER_SEGMENT
#define PAGENO_MAX	MaxMultiXactOffsetOld / MULTIXACT_MEMBERS_PER_PAGE % SLRU_PAGES_PER_SEGMENT
		Assert((oldseg.segno == SEGNO_MAX && oldseg.pageno == PAGENO_MAX + 1) ||
			   (oldseg.segno == SEGNO_MAX + 1 && oldseg.pageno == 0));

		/* Switch to segment 0000 */
		close_segment(&oldseg);
		oldseg.segno = 0;
		oldseg.pageno = 0;

		/* skip invalid zero multi offset */
		oldidx = 1;
	}

	MultiXactMembersCtxFinit(&newctx);

	/* Release resources */
	close_segment(&oldseg);

	pfree(oldseg.dir);
}
