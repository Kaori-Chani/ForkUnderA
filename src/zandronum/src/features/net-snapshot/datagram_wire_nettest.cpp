// SPDX-License-Identifier: GPL-3.0-or-later
//-----------------------------------------------------------------------------
//
// Copyright (C) 2026 Kaori Ishida
//
// Filename: datagram_wire_nettest.cpp
//
// Description: SVC2_SNAPSHOT's header, which capture.cpp builds by hand and from
// whose size SNAP_PACKET_OVERHEAD is derived: nothing tied the two together, so a
// field added or widened here could leave the budget counting the old header.
// Modelled on BYTESTREAM_s rather than the live sender, which resolves per-client
// state and cannot be unit-linked (the netcode-testing skill); faithful because
// NetCommand::addLong, addShort and addByte write the same little-endian bytes.
//
//-----------------------------------------------------------------------------

#include "doomtype.h"						// BYTE and friends, before networkshared.h
#include "networkshared.h"					// BYTESTREAM_s
#include "features/net-snapshot/computation/budget_compute.h"	// SNAP_PACKET_OVERHEAD
#include "wire_nettest.h"					// zx::nettest::Wire

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using zx::nettest::Wire;

namespace
{

// The bytes before the header in an unreliable packet: SVC_UNRELIABLEPACKET, then
// SVC_EXTENDEDCOMMAND and SVC2_SNAPSHOT's own id.
const unsigned int PREFIX_BYTES = 3;
// What SERVER_CheckClientBuffer keeps free past the command (capture.md).
const unsigned int SLACK_BYTES = 5;

// snap_SendDatagram's header, field for field and in its order.
void WriteDatagramHeader(Wire &wire, int frame, int gameTic, int seq, int mapStamp,
	int resetCount, int ackLag, int reliable, int length)
{
	wire.w.WriteLong(frame);
	wire.w.WriteLong(gameTic);
	wire.w.WriteLong(seq);
	wire.w.WriteByte(mapStamp);
	wire.w.WriteByte(resetCount);
	wire.w.WriteByte(ackLag);
	wire.w.WriteShort(reliable);
	wire.w.WriteShort(length);
}

} // namespace

TEST(SnapDatagramWireBytes, TheHeaderIsNineteenBytesAndThePacketBudgetCountsThem)
{
	// The invariant SNAP_PACKET_OVERHEAD stands on: a field added here moves the 27 below,
	// and the byte between 27 and 28 is the only rounding a widening has to spend.
	Wire wire;
	WriteDatagramHeader(wire, 1, 2, 3, 4, 5, 6, 7, 8);

	ASSERT_EQ(wire.written(), 19u);
	const unsigned int counted = PREFIX_BYTES + (unsigned int)wire.written() + SLACK_BYTES;
	EXPECT_EQ(counted, 27u) << "the header changed, so SNAP_PACKET_OVERHEAD needs a look";
	EXPECT_EQ((unsigned int)SNAP_PACKET_OVERHEAD, 28u);
	EXPECT_EQ((unsigned int)SNAP_PACKET_OVERHEAD - counted, 1u)
		<< "the rounding byte, which is what a one-byte widening would spend";
}

TEST(SnapDatagramWireBytes, GoldenTheResetCountIsOneByteBetweenTheStampAndTheLag)
{
	// GOLDEN: the three bytes in the middle of the header, which a reader that mistook any of
	// their widths would shift the two shorts after them by.
	Wire wire;
	WriteDatagramHeader(wire, 0, 0, 0, 0x11, 0x2c, 0x22, 0, 0);

	const std::vector<BYTE> expected = {
		0x00, 0x00, 0x00, 0x00,				// frame
		0x00, 0x00, 0x00, 0x00,				// game tic
		0x00, 0x00, 0x00, 0x00,				// datagram sequence
		0x11,								// map stamp
		0x2c,								// reset count, 44
		0x22,								// ack lag
		0x00, 0x00,							// newest reliable sequence
		0x00, 0x00,							// payload size
	};
	ASSERT_EQ(wire.written(), expected.size());
	EXPECT_EQ(0, memcmp(wire.buf.data(), expected.data(), expected.size()));
}

TEST(SnapDatagramWireBytes, TheHeaderRoundTripsEveryResetCountAByteHolds)
{
	// ROUND-TRIP: every field back in the reader's own widths and order, as OnSnapshot reads
	// them, at the count's ends and either side of the signed turn ResetAhead reads.
	const int counts[] = { 0, 127, 128, 255 };
	for (unsigned int i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
	{
		Wire wire;
		WriteDatagramHeader(wire, -1, 1234, 5678, 0x7F, counts[i], 9, 4321, 1400);
		wire.openReader();

		EXPECT_EQ(wire.r.ReadLong(), -1);
		EXPECT_EQ(wire.r.ReadLong(), 1234);
		EXPECT_EQ(wire.r.ReadLong(), 5678);
		EXPECT_EQ(wire.r.ReadByte(), 0x7F);
		EXPECT_EQ(wire.r.ReadByte(), counts[i]) << "count " << counts[i];
		EXPECT_EQ(wire.r.ReadByte(), 9);
		EXPECT_EQ((WORD)wire.r.ReadShort(), 4321u);
		EXPECT_EQ((WORD)wire.r.ReadShort(), 1400u);
	}
}

TEST(SnapDatagramWireBytes, ACutHeaderReadsInBoundsAndSignalsMinusOne)
{
	// ADVERSARIAL: cut at the reset count and inside the payload size; ASan proves the reads
	// stay in bounds, and -1 is what the reader sees rather than a byte of the next field.
	{
		Wire wire;
		WriteDatagramHeader(wire, 0, 0, 0, 1, 44, 2, 3, 4);
		wire.openReader(13);				// the map stamp arrived, the count did not
		wire.r.pbStream += 12;
		EXPECT_EQ(wire.r.ReadByte(), 1);
		EXPECT_EQ(wire.r.ReadByte(), -1);
	}
	{
		Wire wire;
		WriteDatagramHeader(wire, 0, 0, 0, 1, 44, 2, 3, 4);
		// The payload size is the last two bytes, at 17 and 18.
		wire.openReader(18);				// one byte of the payload size
		wire.r.pbStream += 17;
		EXPECT_EQ(wire.r.ReadShort(), -1);
	}
}
