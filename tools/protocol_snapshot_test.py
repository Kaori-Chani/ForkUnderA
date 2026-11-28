#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kaori Ishida
#
# [Kaori-Chani] Tests for the protocol snapshot's two readers, stdlib only:
#
#   python3 tools/protocol_snapshot_test.py
#
# The hand-framed reader is a regex over C++, so each shape it has to get right is pinned on a
# made-up sender here rather than on sv_commands.cpp, which would drift. The struct and array
# expansion is checked against the real spec, but structurally: that a struct is expanded and an
# array names the Byte count it carries, not what any one member's type is today.
import importlib.util
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("protocol_snapshot",
                                               os.path.join(HERE, "protocol-snapshot.py"))
snap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(snap)


def shapes(text):
    """{id: (flags, fields)} for the blocks a snippet holds, as the golden would print them."""
    out = {}
    for block in snap.hand_blocks(text):
        flags = (" [%s]" % ",".join(block["flags"])) if block["flags"] else ""
        out.setdefault(block["id"], []).append((flags, block["fields"]))
    return out


class HandFramed(unittest.TestCase):
    def test_fields_are_read_in_order_with_their_widths(self):
        text = """
void SERVERCOMMANDS_Thing( void )
{
\tNetCommand command( SVC_THING );
\tcommand.addShort( id );
\tcommand.addByte( kind );
\tcommand.addShortByte( value, 7 );
\tcommand.addInteger( packed, 3 );
\tcommand.addString( name );
\tcommand.addBuffer( body, bytes );
\tcommand.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(text)["SVC_THING"],
                         [("", ["Short", "Byte", "ShortByte<7>", "Integer<3>", "String",
                                "Buffer"])])

    def test_a_field_inside_a_block_is_conditional_and_one_in_a_loop_repeats(self):
        text = """
void SERVERCOMMANDS_Two( void )
{
\tNetCommand command( SVC_TWO );
\tcommand.addByte( kind );
\tif ( thing != NULL )
\t{
\t\tcommand.addShort( thing->id );
\t}
\tfor ( int i = 0; i < n; ++i )
\t{
\t\tcommand.addLong( values[i] );
\t}
\tcommand.addByte( tail );
\tcommand.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(text)["SVC_TWO"],
                         [("", ["Byte", "Short?", "Long*", "Byte"])])

    def test_a_braceless_body_does_not_mark_the_next_block(self):
        text = """
void SERVERCOMMANDS_Plain( void )
{
\tNetCommand command( SVC_PLAIN );
\tfor ( int i = 0; i < n; ++i )
\t\tSkip( i );
\t{
\t\tcommand.addByte( kind );
\t}
\tcommand.sendCommandToClients( extra, flags );
}
"""
        # The loop's body ends at its one statement, so the brace after it opens a plain scope,
        # not the loop: the field is not repeated. A plain scope still reads as conditional,
        # since a "} else {" line opens one and only ? is safe for it.
        self.assertEqual(shapes(text)["SVC_PLAIN"], [("", ["Byte?"])])

    def test_a_braceless_if_body_is_still_conditional(self):
        text = """
void SERVERCOMMANDS_E( void )
{
	NetCommand command( SVC_E );
	command.addByte( x );
	if ( y )
		command.addShort( z );
	command.addByte( tail );
	for ( int i = 0; i < n; ++i )
		command.addLong( each );
	command.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(text)["SVC_E"],
                         [("", ["Byte", "Short?", "Byte", "Long*"])])

    def test_unreliable_is_a_flag_and_a_conditional_one_says_so(self):
        plain = """
void SERVERCOMMANDS_A( void )
{
\tNetCommand command( SVC_A );
\tcommand.addByte( x );
\tcommand.setUnreliable( true );
\tcommand.sendCommandToClients( extra, flags );
}
"""
        gated = """
void SERVERCOMMANDS_B( void )
{
\tNetCommand command( SVC_B );
\tcommand.addByte( x );
\tif ( pActor->NetID == 0 )
\t{
\t\tcommand.setUnreliable( true );
\t}
\tcommand.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(plain)["SVC_A"], [(" [Unreliable]", ["Byte"])])
        self.assertEqual(shapes(gated)["SVC_B"], [(" [Unreliable?]", ["Byte"])])

    def test_two_senders_of_one_id_keep_their_own_shapes(self):
        text = """
void SERVERCOMMANDS_First( void )
{
\tNetCommand command( SVC_ROT );
\tcommand.addByte( SETNEXT );
\tcommand.addShort( position );
\tcommand.sendCommandToClients( extra, flags );
}

void SERVERCOMMANDS_Second( void )
{
\tNetCommand command( SVC_ROT );
\tcommand.addByte( DELENTRY );
\tcommand.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(text)["SVC_ROT"],
                         [("", ["Byte", "Short"]), ("", ["Byte"])])

    def test_a_comment_holding_an_adder_is_not_a_field(self):
        text = """
void SERVERCOMMANDS_C( void )
{
\tNetCommand command( SVC_C );
\t// command.addLong( what this used to send );
\t/* command.addFloat( nor this ); */
\tcommand.addByte( x );
\tcommand.sendCommandToClients( extra, flags );
}
"""
        self.assertEqual(shapes(text)["SVC_C"], [("", ["Byte"])])

    def test_a_write_after_the_function_ends_belongs_to_nothing(self):
        text = """
void SERVERCOMMANDS_D( void )
{
\tNetCommand command( SVC_D );
\tcommand.addByte( x );
\tcommand.sendCommandToClients( extra, flags );
}

void Elsewhere( NetCommand &command )
{
\tcommand.addLong( y );
}
"""
        self.assertEqual(shapes(text)["SVC_D"], [("", ["Byte"])])


class SpecExpansion(unittest.TestCase):
    def test_every_struct_is_expanded_and_every_array_names_its_count(self):
        text = snap.snapshot()
        for line in text.splitlines():
            if " : " not in line:
                continue
            fields = line.split(" : ", 1)[1].split()
            for field in fields:
                # A bare Struct or Array would be the blind spot this pins shut.
                self.assertNotIn(field.split("?")[0], ("Struct", "Array"),
                                 "unexpanded field in: %s" % line)
        self.assertIn("Array<Byte>{Struct<", text)

    def test_the_hand_framed_section_names_the_senders_the_spec_cannot(self):
        text = snap.snapshot()
        self.assertIn("## hand-framed commands", text)
        for cid in ("SVC_DOPUSHER", "SVC2_DOORSOUND", "SVC2_MOVERSOUND", "SVC2_FLOORSOUND",
                    "SVC_DODOOR", "SVC2_SNAPSHOT"):
            self.assertRegex(text, r"(?m)^%s +(\[\w+\??\] )?: \S" % cid)


if __name__ == "__main__":
    unittest.main()
