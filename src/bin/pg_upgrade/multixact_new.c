/*
 * multixact_new.c
 *
 * Rewrite pre-v19 multixacts to new format with 64-bit MultiXactOffsets
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_new.c
 */

#include "multixact_new.h"

/*
 * NOTE: Below are a bunch of definitions and simple inline functions that are
 * copy-pasted from multixact.c
 */

/* We need four bytes per offset, 8 bytes for the base */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

static inline int64
MultiXactIdToOffsetPage(MultiXactId multi)
{
	return multi / MULTIXACT_OFFSETS_PER_PAGE;
}

static inline int
MultiXactIdToOffsetEntry(MultiXactId multi)
{
	return multi % MULTIXACT_OFFSETS_PER_PAGE;
}

/* We need eight bits per xact, so one xact fits in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/*
 * Because the number of items per page is not a divisor of the last item
 * number (member 0xFFFFFFFF), the last segment does not use the maximum number
 * of pages, and moreover the last used page therein does not use the same
 * number of items as previous pages.  (Another way to say it is that the
 * 0xFFFFFFFF member is somewhere in the middle of the last page, so the page
 * has some empty space after that item.)
 *
 * This constant is the number of members in the last page of the last segment.
 */
#define MAX_MEMBERS_IN_LAST_MEMBERS_PAGE \
		((uint32) ((0xFFFFFFFF % MULTIXACT_MEMBERS_PER_PAGE) + 1))

/* page in which a member is to be found */
static inline int64
MXOffsetToMemberPage(MultiXactOffset offset)
{
	return offset / MULTIXACT_MEMBERS_PER_PAGE;
}

/* Location (byte offset within page) of flag word for a given member */
static inline int
MXOffsetToFlagsOffset(MultiXactOffset offset)
{
	MultiXactOffset group = offset / MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			grouponpg = group % MULTIXACT_MEMBERGROUPS_PER_PAGE;
	int			byteoff = grouponpg * MULTIXACT_MEMBERGROUP_SIZE;

	return byteoff;
}

static inline int
MXOffsetToFlagsBitShift(MultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			bshift = member_in_group * MXACT_MEMBER_BITS_PER_XACT;

	return bshift;
}

/* Location (byte offset within page) of TransactionId of given member */
static inline int
MXOffsetToMemberOffset(MultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;

	return MXOffsetToFlagsOffset(offset) +
		MULTIXACT_FLAGBYTES_PER_GROUP +
		member_in_group * sizeof(TransactionId);
}

static inline void
MXOffsetWrite(char *buf, int entryno, MultiXactOffset offset)
{
	MultiXactOffset *offptr = (MultiXactOffset *) buf;

	offptr[entryno] = offset;
}

MultiXactWriter *
AllocMultiXactWrite(char *pgdata, MultiXactId firstMulti,
					MultiXactOffset firstOffset)
{
	MultiXactWriter    *state = pg_malloc(sizeof(*state));
	char				dir[MAXPGPATH] = {0};

	state->nextMXact = firstMulti;
	state->nextOffset = firstOffset;

	pg_sprintf(dir, "%s/pg_multixact/offsets", pgdata);
	state->offset = AllocSlruWrite(dir, false);

	pg_sprintf(dir, "%s/pg_multixact/members", pgdata);
	state->members = AllocSlruWrite(dir, true /* use long segment names */);

	return state;
}

/*
 * Simplified copy of the corresponding server function
 */
MultiXactId
GetNewMultiXactId(MultiXactWriter *state, int nmembers, MultiXactOffset *offset)
{
	MultiXactId		result;

	/* Handle wraparound of the nextMXact counter */
	if (state->nextMXact < FirstMultiXactId)
		state->nextMXact = FirstMultiXactId;

	/* Assign the MXID */
	result = state->nextMXact;

	/* Reserve the members space, similarly to above. */
	*offset = state->nextOffset;

	/*
	 * Advance counters.  As in GetNewTransactionId(), this must not happen
	 * until after file extension has succeeded!
	 *
	 * We don't care about MultiXactId wraparound here; it will be handled by
	 * the next iteration.  But note that nextMXact may be InvalidMultiXactId
	 * or the first value on a segment-beginning page after this routine
	 * exits, so anyone else looking at the variable must be prepared to deal
	 * with either case.  Similarly, nextOffset may be zero, but we won't use
	 * that as the actual start offset of the next multixact.
	 */
	(state->nextMXact)++;

	state->nextOffset += nmembers;

	return result;
}

/*
 * Write a new multixact with members.
 *
 * Simplified version of the correspoding server function, hence the name.
 */
void
RecordNewMultiXact(MultiXactWriter *state, MultiXactOffset offset,
				   MultiXactId multi, int nmembers, MultiXactMember *members)
{
	int64		pageno;
	int64		prev_pageno;
	int			entryno,
				i;
	char	   *buf;

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	buf = SlruWriteSwitchPage(state->offset, pageno);
	MXOffsetWrite(buf, entryno, offset);

	prev_pageno = -1;

	for (i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			buf = SlruWriteSwitchPage(state->members, pageno);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *) (buf + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *) (buf + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;
	}
}

void
FreeMultiXactWrite(MultiXactWriter *state)
{
	FreeSlruWrite(state->offset);
	FreeSlruWrite(state->members);

	pfree(state);
}
