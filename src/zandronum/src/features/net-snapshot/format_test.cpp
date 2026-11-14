//-----------------------------------------------------------------------------
//
// Copyright (C) 2026 Kaori Ishida
//
// Filename: format_test.cpp
//
// Description: Tests for format.h's position frame: the extent rule, every
// position field on both sides of a nonzero origin at the narrow limits and
// one past them, the checksum's narrow pin, and the same again on a frame
// wide in x and y which no 32-bit build ever produced.
//
//-----------------------------------------------------------------------------

#include "format.h"
#include <gtest/gtest.h>

#include <string.h>

#include "doomdef.h"		// TICRATE

using namespace zx::netsnapshot;

// [Kaori-Chani] The checksum block's inputs, the same on every compiler, unlike rand(), whose 15
// bits on MSVC never reached the sign bit or a rounding half.
#define FORMATTEST_SEED		0x9E3779B9u

static DWORD formattest_Next(DWORD& s)
{
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

// [Kaori-Chani] A frame with a nonzero origin, which a 32-bit fixed_t never produced, so only
// this runs the offset paths. x spans [-32668, 32867] and y [-32868, 32667], so each axis has one
// limit a 16.16 fixed_t could reach; a 48.16 one reaches both, and the cases below use the
// reachable ones either way.
static SnapFrame formattest_OffsetFrame(unsigned int* pClamped)
{
	SnapFrame f = FrameNarrow();
	f.origin[0] = 100;
	f.origin[1] = -100;
	f.origin[2] = 7;
	f.pClamped = pClamped;
	return f;
}

// [Kaori-Chani] A frame wide in x and y, narrow in z, placed so each wide axis has one limit a
// 16.16 fixed_t reaches: x spans [-1000, 16776215] and y [-16776216, 999].
static SnapFrame formattest_WideFrame(unsigned int* pClamped)
{
	SnapFrame f = FrameNarrow();
	f.origin[0] = 8387608;
	f.origin[1] = -8387608;
	f.origin[2] = 7;
	f.width[0] = SNAP_FRAME_WIDE;
	f.width[1] = SNAP_FRAME_WIDE;
	f.pClamped = pClamped;
	return f;
}

static const fixed_t FORMATTEST_XLO = -32668 * FRACUNIT;
// [Kaori-Chani] The top of y's 16.16 range from the origin, which a 48.16 fixed_t still holds.
static const fixed_t FORMATTEST_YHI = -100 * FRACUNIT + (fixed_t)0x7FFFFFFF;

// [Kaori-Chani] Origin 0 while the extent and its clamped margin fit plus or minus 32k, else
// centred, wide past 65,535 units and overflowing past 16,777,215.
TEST(SnapshotFormat, TheExtentDecidesTheFramesOriginAndWidth)
{
	const long long lo[3] = { -1000, -30000, 0 }, hi[3] = { 1000, 30000, 100 };
	bool bOver = true;
	const SnapFrame a = FrameFromExtent(lo, hi, -32768, 32767, &bOver);
	EXPECT_EQ(0, a.origin[0]);
	EXPECT_EQ(0, a.origin[1]);
	EXPECT_EQ(0, a.origin[2]);
	EXPECT_FALSE(bOver);
	EXPECT_EQ(SNAP_FRAME_NARROW, a.width[0]);
	EXPECT_EQ(SNAP_FRAME_NARROW, a.width[1]);

	const long long lo2[3] = { 10000, -50000, -5000 }, hi2[3] = { 60000, 100000, 0 };
	bool bOver2 = true;
	const SnapFrame b = FrameFromExtent(lo2, hi2, -0x80000000LL, 0x7FFFFFFFLL, &bOver2);
	EXPECT_EQ(35000, b.origin[0]);
	EXPECT_EQ(25000, b.origin[1]);
	EXPECT_EQ(0, b.origin[2]);
	EXPECT_FALSE(bOver2);
	EXPECT_EQ(SNAP_FRAME_NARROW, b.width[0]);
	EXPECT_EQ(SNAP_FRAME_WIDE, b.width[1]);
	EXPECT_EQ(SNAP_FRAME_NARROW, b.width[2]);
	EXPECT_EQ(NULL, b.pClamped);

	const long long lo3[3] = { -10000000, 0, 0 }, hi3[3] = { 10000000, 0, 0 };
	bool bOver3 = false;
	const SnapFrame c = FrameFromExtent(lo3, hi3, -0x80000000LL, 0x7FFFFFFFLL, &bOver3);
	EXPECT_EQ(0, c.origin[0]);
	EXPECT_EQ(SNAP_FRAME_WIDE, c.width[0]);
	EXPECT_TRUE(bOver3) << "an axis past 16,777,215 units overflows a wide frame";
}

// [Kaori-Chani] Rounded and floored, each clamped at both ends and counted; the floored values
// relative to the origin as the point sound sends them.
TEST(SnapshotFormat, TheFramesQuantizersRoundFloorAndClamp)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_OffsetFrame(&clamps);

	EXPECT_EQ(-32668, QuantPosIn(f, 0, FORMATTEST_XLO));
	EXPECT_EQ(-32668, QuantPosIn(f, 0, FORMATTEST_XLO - FRACUNIT / 2 - 1));
	EXPECT_EQ(32667, QuantPosIn(f, 1, 32667 * FRACUNIT + FRACUNIT / 2 - 1));
	EXPECT_EQ(32667, QuantPosIn(f, 1, 32667 * FRACUNIT + FRACUNIT / 2));

	EXPECT_EQ(-32668, FloorPosIn(f, 0, FORMATTEST_XLO));
	EXPECT_EQ(-32668, FloorPosIn(f, 0, FORMATTEST_XLO - 1));
	EXPECT_EQ(32667, FloorPosIn(f, 1, 32668 * FRACUNIT - 1));
	EXPECT_EQ(32667, FloorPosIn(f, 1, 32668 * FRACUNIT));

	// [Kaori-Chani] 99.5 floors to 99, one below the origin; -0.5 to -1; 100.5 to the origin.
	const fixed_t v[3] =
		{ 99 * FRACUNIT + FRACUNIT / 2, -FRACUNIT / 2, 100 * FRACUNIT + FRACUNIT / 2 };
	const int rel[3] = { -1, -101, 0 };
	for (unsigned int i = 0; i < 3; ++i)
	{
		const SDWORD r = PosRel(f, 0, FloorPosIn(f, 0, v[i]));
		EXPECT_EQ(rel[i], r) << "case " << i;
		EXPECT_EQ(v[i] & ~(FRACUNIT - 1), DequantPos(PosAbs(f, 0, r))) << "case " << i;
	}

	EXPECT_EQ(4u, clamps) << "two rounded and two floored values sit past a limit";
}

// [Kaori-Chani] The values are already in the frame, so nothing clamps: the limits, and either
// side of the origin.
TEST(SnapshotFormat, TheFramesGroupsAndBranchEventRoundTrip)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_OffsetFrame(&clamps);
	const SDWORD xs[] = { -32668, 99, 100, 101, 32867 };
	const SDWORD ys[] = { -32868, -101, -100, -99, 32667 };
	const SDWORD zs[] = { -32761, 6, 7, 8, 32774 };

	for (unsigned int i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i)
	{
		SnapEntityState e, r;
		memset(&e, 0, sizeof(e));
		memset(&r, 0, sizeof(r));
		e.qx = xs[i];
		e.qy = ys[i];
		e.qz = zs[i];
		BYTE buf[SNAP_MAX_EVENT_BYTES];
		WriteGroups(buf, f, e, (BYTE)(SNAPGRP_POS | SNAPGRP_POSZ));
		ReadGroups(buf, f, r, (BYTE)(SNAPGRP_POS | SNAPGRP_POSZ));
		EXPECT_EQ((WORD)(SWORD)(xs[i] - 100), Read16(buf)) << "case " << i;
		EXPECT_EQ((WORD)(SWORD)(ys[i] + 100), Read16(buf + 2)) << "case " << i;
		EXPECT_EQ(e.qx, r.qx) << "case " << i;
		EXPECT_EQ(e.qy, r.qy) << "case " << i;
		EXPECT_EQ(e.qz, r.qz) << "case " << i;

		memset(&r, 0, sizeof(r));
		WriteEventBody(buf, f, SNAPEV_BRANCH, e, 0);
		ReadEvent(buf + SNAP_EVENT_MASK_BYTES, f, SNAPEV_BRANCH, r);
		EXPECT_EQ(e.qx, r.qx) << "branch, case " << i;
		EXPECT_EQ(e.qy, r.qy) << "branch, case " << i;
		EXPECT_EQ(e.qz, r.qz) << "branch, case " << i;
	}
	EXPECT_EQ(0u, clamps);
}

// [Kaori-Chani] 16.16 relative to the origin: the limits exactly, a fraction either side of the
// origin, and one step past each limit, which clamps to it.
TEST(SnapshotFormat, TheFramesLocalBlockAndPolyobjectClampAtTheirLimits)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_OffsetFrame(&clamps);
	BYTE buf[SNAP_MAX_EVENT_BYTES];
	SnapLocalState s, b;
	memset(&s, 0, sizeof(s));
	s.x = FORMATTEST_XLO;
	s.y = FORMATTEST_YHI;
	s.z = 7 * FRACUNIT - 1;
	memset(&b, 0, sizeof(b));

	WriteLocalBody(buf, f, s);
	ReadLocal(buf + SNAP_EVENT_MASK_BYTES, f, b);
	EXPECT_EQ(s.x, b.x);
	EXPECT_EQ(s.y, b.y);
	EXPECT_EQ(s.z, b.z);
	EXPECT_EQ(0u, clamps);

	s.x = FORMATTEST_XLO - 1;
	s.y = FORMATTEST_YHI + 1;
	WriteLocalBody(buf, f, s);
	ReadLocal(buf + SNAP_EVENT_MASK_BYTES, f, b);
	EXPECT_EQ(FORMATTEST_XLO, b.x);
	EXPECT_EQ(FORMATTEST_YHI, b.y);
	EXPECT_EQ(2u, clamps);

	SnapMoverPoly a, p;
	memset(&a, 0, sizeof(a));
	a.x = 99 * FRACUNIT + FRACUNIT / 2;
	a.y = -101 * FRACUNIT;
	a.count = 1;
	a.e[0].kind = SNAPPO_MOVETO;
	a.e[0].xTarget = FORMATTEST_XLO - 1;
	a.e[0].yTarget = 12345;
	BYTE body[SNAPMV_BODY_MAX];
	const unsigned int n = WriteMoverPoly(body, f, a);
	memset(&p, 0, sizeof(p));
	EXPECT_EQ(0xFFFF8000u, Read32(body));
	ASSERT_TRUE(ReadMoverPoly(body, f, n, p));
	EXPECT_EQ(a.x, p.x);
	EXPECT_EQ(a.y, p.y);
	EXPECT_EQ(FORMATTEST_XLO, p.e[0].xTarget);
	EXPECT_EQ(12345, p.e[0].yTarget);
	EXPECT_EQ(3u, clamps);
}

// [Kaori-Chani] Floored before the origin is taken, so a fraction below the origin is -1, and a
// fraction below 0 is the unit under it.
TEST(SnapshotFormat, TheFramesCosmeticEventsFloorBeforeTheOrigin)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_OffsetFrame(&clamps);
	BYTE buf[40];
	bool bBig, bSmall;
	SnapCosGeneric g, gr;
	memset(&g, 0, sizeof(g));
	g.x = 99 * FRACUNIT + FRACUNIT / 2;
	g.y = -100 * FRACUNIT - FRACUNIT / 4;
	g.z = 7 * FRACUNIT;
	g.stateCode = -1;
	const unsigned int gn = CosWriteGeneric(buf, f, SNAP_COSE_PLAIN, g, bBig, bSmall);
	SnapCosCursor gc = { buf + 1, buf + gn };
	CosReadGeneric(gc, f, buf[0], gr);
	EXPECT_EQ(0xFFFF, Read16(buf + 3));
	EXPECT_EQ(0xFFFF, Read16(buf + 5));
	EXPECT_EQ(0, Read16(buf + 7));
	EXPECT_EQ(99 * FRACUNIT, gr.x);
	EXPECT_EQ(-101 * FRACUNIT, gr.y);
	EXPECT_EQ(7 * FRACUNIT, gr.z);
	EXPECT_EQ(0u, clamps);

	SnapCosBlood c, cr;
	memset(&c, 0, sizeof(c));
	c.x = FORMATTEST_XLO - 1;
	c.y = 32667 * FRACUNIT + FRACUNIT - 1;
	c.z = -1;
	const unsigned int cn = CosWriteBlood(buf, f, SNAP_COSE_SPLATTER, c);
	SnapCosCursor cc = { buf + 1, buf + cn };
	CosReadBlood(cc, f, buf[0], cr);
	EXPECT_EQ(FORMATTEST_XLO, cr.x);
	EXPECT_EQ(32667 * FRACUNIT, cr.y);
	EXPECT_EQ(-FRACUNIT, cr.z);
	EXPECT_EQ(1u, clamps);
}

// [Kaori-Chani] A narrow position hashes as it did before the frame existed, pinned over 4,096
// drawn states, and a wide one differs from the narrow value sharing its low 16 bits.
TEST(SnapshotFormat, TheStateChecksumKeepsItsNarrowPin)
{
	DWORD seed = FORMATTEST_SEED, acc = 0;
	for (int i = 0; i < 4096; ++i)
	{
		SnapEntityState e;
		memset(&e, 0, sizeof(e));
		e.qx = (SWORD)formattest_Next(seed);
		e.qy = (SWORD)formattest_Next(seed);
		e.qz = (SWORD)formattest_Next(seed);
		e.qangle = (WORD)formattest_Next(seed);
		e.qpitch = (WORD)formattest_Next(seed);
		e.qvx = (SWORD)formattest_Next(seed);
		e.qvy = (SWORD)formattest_Next(seed);
		e.qvz = (SWORD)formattest_Next(seed);
		e.qstate = (WORD)formattest_Next(seed);
		e.renderstate = formattest_Next(seed);
		e.effects = (BYTE)formattest_Next(seed);
		e.misc = (WORD)formattest_Next(seed);
		acc = (acc * 31u) ^ StateChecksum(e);
	}
	EXPECT_EQ(0xEBA815A2u, acc);

	SnapEntityState w, n;
	memset(&w, 0, sizeof(w));
	memset(&n, 0, sizeof(n));
	w.qx = 40000;
	n.qx = 40000 - 65536;
	EXPECT_NE(StateChecksum(w), StateChecksum(n))
		<< "a wide position must not hash as the narrow one sharing its low 16 bits";
}

static const fixed_t FORMATTEST_WIDE_XLO = -1000 * FRACUNIT;
static const fixed_t FORMATTEST_WIDE_YHI = 999 * FRACUNIT + (FRACUNIT - 1);

// [Kaori-Chani] The limits on the wide axes, rounded and floored, and one past each.
TEST(SnapshotFormat, AWideFramesQuantizersRoundFloorAndClamp)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_WideFrame(&clamps);

	EXPECT_EQ(-1000, QuantPosIn(f, 0, FORMATTEST_WIDE_XLO));
	EXPECT_EQ(-1000, QuantPosIn(f, 0, FORMATTEST_WIDE_XLO - FRACUNIT / 2 - 1));
	EXPECT_EQ(999, QuantPosIn(f, 1, 999 * FRACUNIT + FRACUNIT / 2 - 1));
	EXPECT_EQ(999, QuantPosIn(f, 1, 999 * FRACUNIT + FRACUNIT / 2));

	EXPECT_EQ(-1000, FloorPosIn(f, 0, FORMATTEST_WIDE_XLO));
	EXPECT_EQ(-1000, FloorPosIn(f, 0, FORMATTEST_WIDE_XLO - 1));
	EXPECT_EQ(999, FloorPosIn(f, 1, FORMATTEST_WIDE_YHI));
	EXPECT_EQ(999, FloorPosIn(f, 1, FORMATTEST_WIDE_YHI + 1));

	EXPECT_EQ(4u, clamps);
}

// [Kaori-Chani] Three bytes an axis on x and y, two on z, at the limits and either side of the
// origin, through the writer, the three length functions and the reader.
TEST(SnapshotFormat, AWideFramesGroupsAndBranchEventRoundTripInThreeBytes)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_WideFrame(&clamps);
	const SDWORD xs[] = { -1000, 8387607, 8387608, 8387609, 16776215 };
	const SDWORD ys[] = { -16776216, -8387609, -8387608, -8387607, 999 };
	const SDWORD zs[] = { -32761, 6, 7, 8, 32774 };
	const BYTE mask = (BYTE)(SNAPGRP_POS | SNAPGRP_POSZ);

	EXPECT_EQ(8u, GroupBytes(f, mask));
	EXPECT_EQ(8u, GroupBytesBeforeAppear(f, mask));
	EXPECT_EQ((unsigned int)(SNAP_EVBYTES_BRANCH + 2), EventBytes(f, SNAPEV_BRANCH));

	for (unsigned int i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i)
	{
		SnapEntityState e, r;
		memset(&e, 0, sizeof(e));
		memset(&r, 0, sizeof(r));
		e.qx = xs[i];
		e.qy = ys[i];
		e.qz = zs[i];
		BYTE buf[SNAP_MAX_EVENT_BYTES];
		const unsigned int n = WriteGroups(buf, f, e, mask);
		unsigned int need = 0;
		EXPECT_EQ(8u, n) << "case " << i;
		EXPECT_EQ(8u, GroupBytesFor(f, mask, e)) << "case " << i;
		EXPECT_TRUE(GroupBytesRead(f, mask, buf, n, need)) << "case " << i;
		EXPECT_EQ(8u, need) << "case " << i;
		EXPECT_FALSE(GroupBytesRead(f, mask, buf, n - 1, need)) << "case " << i;
		EXPECT_EQ(xs[i] - 8387608, ReadSigned(buf, 3)) << "case " << i;
		EXPECT_EQ(ys[i] + 8387608, ReadSigned(buf + 3, 3)) << "case " << i;
		EXPECT_EQ(zs[i] - 7, ReadSigned(buf + 6, 2)) << "case " << i;

		ReadGroups(buf, f, r, mask);
		EXPECT_EQ(e.qx, r.qx) << "case " << i;
		EXPECT_EQ(e.qy, r.qy) << "case " << i;
		EXPECT_EQ(e.qz, r.qz) << "case " << i;

		memset(&r, 0, sizeof(r));
		const unsigned int en = WriteEventBody(buf, f, SNAPEV_BRANCH, e, 0);
		ReadEvent(buf + SNAP_EVENT_MASK_BYTES, f, SNAPEV_BRANCH, r);
		EXPECT_EQ(SNAP_EVENT_MASK_BYTES + EventBytes(f, SNAPEV_BRANCH), en) << "case " << i;
		EXPECT_EQ(e.qx, r.qx) << "branch, case " << i;
		EXPECT_EQ(e.qy, r.qy) << "branch, case " << i;
		EXPECT_EQ(e.qz, r.qz) << "branch, case " << i;
	}
	EXPECT_EQ(0u, clamps);
}

// [Kaori-Chani] Five bytes of 16.16 on a wide axis: the limits exactly, then one step past each,
// which clamps to it.
TEST(SnapshotFormat, AWideFramesLocalBlockAndPolyobjectTakeFiveBytesAnAxis)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_WideFrame(&clamps);
	BYTE buf[SNAP_MAX_EVENT_BYTES];
	SnapLocalState s, b;
	memset(&s, 0, sizeof(s));
	s.x = FORMATTEST_WIDE_XLO;
	s.y = FORMATTEST_WIDE_YHI;
	s.z = 7 * FRACUNIT - 1;
	s.jumpTics = -2;
	memset(&b, 0, sizeof(b));

	const unsigned int n = WriteLocalBody(buf, f, s);
	ReadLocal(buf + SNAP_EVENT_MASK_BYTES, f, b);
	EXPECT_EQ(SNAP_EVENT_MASK_BYTES + EventBytes(f, SNAPEV_LOCAL), n);
	EXPECT_EQ((unsigned int)(SNAP_EVENT_MASK_BYTES + SNAP_EVBYTES_LOCAL + 2), n);
	EXPECT_EQ(s.x, b.x);
	EXPECT_EQ(s.y, b.y);
	EXPECT_EQ(s.z, b.z);
	EXPECT_EQ(s.jumpTics, b.jumpTics);
	EXPECT_EQ(0u, clamps);

	s.x = FORMATTEST_WIDE_XLO - 1;
	s.y = FORMATTEST_WIDE_YHI + 1;
	WriteLocalBody(buf, f, s);
	ReadLocal(buf + SNAP_EVENT_MASK_BYTES, f, b);
	EXPECT_EQ(FORMATTEST_WIDE_XLO, b.x);
	EXPECT_EQ(FORMATTEST_WIDE_YHI, b.y);
	EXPECT_EQ(2u, clamps);

	SnapMoverPoly a, p;
	memset(&a, 0, sizeof(a));
	a.x = FORMATTEST_WIDE_XLO;
	a.y = FORMATTEST_WIDE_YHI;
	a.angle = 0x40000000u;
	a.count = 2;
	a.e[0].kind = SNAPPO_MOVETO;
	a.e[0].xTarget = FORMATTEST_WIDE_XLO - 1;
	a.e[0].yTarget = 12345;
	a.e[1].kind = SNAPPO_ROTATE;
	a.e[1].speed = -7;
	BYTE body[SNAPMV_BODY_MAX];
	const unsigned int bn = WriteMoverPoly(body, f, a);
	const unsigned int want = PolyHeaderBytes(f) + PolyEntryBytes(f, SNAPPO_MOVETO)
		+ PolyEntryBytes(f, SNAPPO_ROTATE);
	memset(&p, 0, sizeof(p));
	EXPECT_EQ(want, bn);
	EXPECT_EQ((unsigned int)(SNAPPO_HEADER_BYTES + 2 + SNAPPO_MOVETO_BYTES + 2
		+ SNAPPO_ENTRY_BYTES), want);
	EXPECT_EQ(bn, MoverPolyBodyBytes(f, body, bn));
	EXPECT_EQ(0u, MoverPolyBodyBytes(f, body, bn - 1)) << "one byte short reads as no body";
	ASSERT_TRUE(ReadMoverPoly(body, f, bn, p));
	EXPECT_EQ(a.x, p.x);
	EXPECT_EQ(a.y, p.y);
	EXPECT_EQ(a.angle, p.angle);
	EXPECT_EQ(2u, p.count);
	EXPECT_EQ(FORMATTEST_WIDE_XLO, p.e[0].xTarget);
	EXPECT_EQ(12345, p.e[0].yTarget);
	EXPECT_EQ(-7, p.e[1].speed);
	EXPECT_EQ(3u, clamps);
}

// [Kaori-Chani] Floored, then relative in three bytes: the limits exactly, and one past each on a
// blood event, which clamps.
TEST(SnapshotFormat, AWideFramesCosmeticEventsTakeThreeBytesAnAxis)
{
	unsigned int clamps = 0;
	const SnapFrame f = formattest_WideFrame(&clamps);
	BYTE buf[SNAP_COSE_GENERIC_MAX + 8];
	bool bBig, bSmall;
	SnapCosGeneric g, gr;
	memset(&g, 0, sizeof(g));
	g.x = FORMATTEST_WIDE_XLO + FRACUNIT / 2;
	g.y = FORMATTEST_WIDE_YHI;
	g.z = 7 * FRACUNIT;
	g.stateCode = -1;
	const unsigned int gn = CosWriteGeneric(buf, f, SNAP_COSE_PLAIN, g, bBig, bSmall);
	SnapCosCursor gc = { buf + 1, buf + gn };
	CosReadGeneric(gc, f, buf[0], gr);
	EXPECT_EQ(13u + 2u, gn);
	EXPECT_EQ(gc.end, gc.p);
	EXPECT_EQ(-1000 - 8387608, ReadSigned(buf + 3, 3));
	EXPECT_EQ(999 + 8387608, ReadSigned(buf + 6, 3));
	EXPECT_EQ(0, ReadSigned(buf + 9, 2));
	EXPECT_EQ(FORMATTEST_WIDE_XLO, gr.x);
	EXPECT_EQ(999 * FRACUNIT, gr.y);
	EXPECT_EQ(7 * FRACUNIT, gr.z);
	EXPECT_EQ(0u, clamps);

	// [Kaori-Chani] Every generic field, so the longest body is the constant that sizes it.
	g.vel[0] = FRACUNIT / 2;
	g.stateCode = 0x3005;
	g.extras = SNAP_COSX_ACTED | SNAP_COSX_TRANSLATION | SNAP_COSX_ALPHA | SNAP_COSX_SCALE
		| SNAP_COSX_STYLE;
	const SnapFrame widest = FrameWidest();
	EXPECT_EQ((unsigned int)SNAP_COSE_GENERIC_MAX,
		CosWriteGeneric(buf, widest, SNAP_COSE_PLAIN, g, bBig, bSmall));

	SnapCosBlood c, cr;
	memset(&c, 0, sizeof(c));
	c.x = FORMATTEST_WIDE_XLO - 1;
	c.y = FORMATTEST_WIDE_YHI + 1;
	c.z = -1;
	const unsigned int cn = CosWriteBlood(buf, f, SNAP_COSE_SPLATTER, c);
	SnapCosCursor cc = { buf + 1, buf + cn };
	CosReadBlood(cc, f, buf[0], cr);
	EXPECT_EQ(9u + 2u, cn);
	EXPECT_EQ(cc.end, cc.p);
	EXPECT_EQ(FORMATTEST_WIDE_XLO, cr.x);
	EXPECT_EQ(999 * FRACUNIT, cr.y);
	EXPECT_EQ(-FRACUNIT, cr.z);
	EXPECT_EQ(2u, clamps);

	// [Kaori-Chani] One byte short reads past the end, as a truncated event is found.
	SnapCosCursor cut = { buf + 1, buf + cn - 1 };
	CosReadBlood(cut, f, buf[0], cr);
	EXPECT_GT(cut.p, cut.end);
}
