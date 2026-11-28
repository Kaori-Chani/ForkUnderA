// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kaori Ishida
//
// [Kaori-Chani] widemap.wad: the map the protocol's widths are measured on -- more sectors and
// more lines than a signed short can index, and a span wider than one can reach.
//
//   node src/zandronum/src/features/net-snapshot/testdata/mkwidemap.mjs
//
// MAP36, a slot no stock game fills (Freedoom 2 ends at MAP32 and our other fixtures take 33 to
// 35), so loading it proves it is this map. 289 blocks of 11 x 11 cells fill it -- 34,969 sectors
// and 76,296 lines before anything under test is added -- spanning 58,752 units in y, from
// -29,376 to 29,376, and 61,920 in x, because the start room stands clear of the grid to the
// east and reaches 32,544. The module adds SNAP_FRAME_MARGIN_XY each side, so an x extent
// past 61,439 units takes that axis to three bytes on the wire while y stays at two: the
// per-axis width runs live, and mixed, which no other fixture does.
//
// It stops at 32,544 rather than reaching past 32,767 because the UDMF loader is still
// 16.16, whatever fixed_t holds: FLOAT2FIXED is xs_Fix<16>, whose Fix is an int32, so a
// vertex (p_udmf.cpp:1681) or a thing (p_udmf.cpp:491) past 32,767.99 units wraps silently --
// 40,176 loads as -25,360. The widest map this engine can load is therefore 65,534 units,
// and a span past 65,535 waits on that.
//
// The blocks stand a cell apart rather than wall to wall, because the engine walks connected
// sectors by recursion: one region of 32,896 sectors overflowed the stack twice over before this
// map would open, in P_FloodZone (p_setup.cpp) and in DoSetMapSection (features/levelprep). A
// block of 121 cells is deep enough for neither. Each block is walkable inside itself.
//
// What is under test is allocated in two passes around those blocks, because an index is its
// allocation order: the low pass before them holds a door and a switch a short can address, and
// the high pass after them holds the same two past 32,767, plus the point pusher's sector. One
// walk and two presses in the start room therefore exercise both sides of every width at once.
//
// The start room stands 30,000 units and more from the origin, so a pusher a client centres on
// the origin pushes nothing at all there rather than pushing slightly wrong. DPusher's int
// centre still holds 30,336 units as 16.16, and cannot be made to overflow while the loader
// caps a coordinate at 32,767.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { UdmfMap, wad, mapLumps } from "../../../../../../tools/fuactl/src/udmf.mjs";

const DOOR_RAISE = 12;              // Door_Raise (tag, speed, delay, lighttag)
const POINTPUSH_SETFORCE = 227;     // PointPush_SetForce (tag, tid, magnitude, line)
const LINE_SETBLOCKING = 55;        // Line_SetBlocking (lineid, setflags, clearflags)
const THING_POINTPUSHER = 5001;     // PointPusher, found by the sector it stands in

const TEX = { texturefloor: "FLOOR4_8", textureceiling: "CEIL3_5", lightlevel: 192 };
const WALL = { texturemiddle: "STARTAN2" };
// SW1GARG rather than a prettier one: animdefs puts the STARTAN switches in the Doom 1
// group, which a Doom 2 IWAD skips, so pressing one of those swaps nothing and sends nothing.
const SWITCH = { texturemiddle: "SW1GARG" };
const OPEN = { texturemiddle: "-" };
const DOORSIDE = { texturemiddle: "-", texturetop: "STARTAN2", texturebottom: "STARTAN2" };
const FLAT = { heightfloor: 0, heightceiling: 128, ...TEX };

// A filler cell carries no textures and no floor height: nothing reaches it, and the properties
// left out save four megabytes of TEXTMAP over 34,969 of them.
const CELL_SECTOR = { heightceiling: 128 };

const CELL = 288;
const BLOCK = 11;                   // cells each way in one block, its own connected region
const PITCH = BLOCK + 1;            // the solid cell between blocks
const BLOCKS = 17;                  // 17 x 17 of them, every one built
const X0 = -(BLOCKS * PITCH * CELL) / 2;

const m = new UdmfMap({ namespace: "zdoom" });

const at = (n) => X0 + n * CELL;

// The start room stands on its own 576 units east of the grid, which takes the x extent past
// the 61,439 units that make an axis wide while every coordinate stays loadable.
const RX0 = 29952;
const RY0 = -1296;
const RX1 = 32544;
const RY1 = 1296;
const RMX = (RX0 + RX1) / 2;

// ---------------------------------------------------------------- the low pass
// Allocated first, so a short addresses all of it: the start room, the door tag 1 standing in it,
// and the switch in the north wall that opens that door.
const start = m.sector(FLAT);
const lowDoor = m.block(RX0 + 256, RY1 - 512, RX0 + 576, RY1 - 192,
  { heightfloor: 0, heightceiling: 0, ...TEX, id: 1 }, start, () => DOORSIDE, () => DOORSIDE);

// The perimeter, clockwise, each wall one-sided with the room on its right. The north wall's
// middle is the low switch; the south wall's is left for the high pass.
m.wall(RX0, RY0, RX0, RY1, start, WALL);                  // west
m.wall(RX0, RY1, RMX - 128, RY1, start, WALL);            // north, west of the switch
const lowSwitch = m.wall(RMX - 128, RY1, RMX + 128, RY1, start, SWITCH,
  { special: DOOR_RAISE, arg0: 1, arg1: 16, arg2: 140, playeruse: true, repeatspecial: true });
m.wall(RMX + 128, RY1, RX1, RY1, start, WALL);            // north, east of the switch
m.wall(RX1, RY1, RX1, RY0, start, WALL);                  // east
m.wall(RX1, RY0, RMX + 128, RY0, start, WALL);            // south, east of the high switch
m.wall(RMX - 128, RY0, RX0, RY0, start, WALL);            // south, west of it

// The walkover that opens the high door, and the line that gives the pusher its force. Both sit
// in the low pass: what is under test there is the sector each one names, not the line itself.
m.opening(RX0 + 64, RY0 + 576, RX1 - 64, RY0 + 576, start, start, OPEN, OPEN,
  { special: DOOR_RAISE, arg0: 2, arg1: 16, arg2: 140, playercross: true, repeatspecial: true,
    blocking: false });
m.opening(RX0 + 64, RY0 + 384, RX1 - 64, RY0 + 384, start, start, OPEN, OPEN,
  { special: POINTPUSH_SETFORCE, arg0: 3, arg1: 0, arg2: 200, arg3: 0, blocking: false });

// A second line change, so both line-carrying commands are driven: the switch above sends
// a texture row, and this clears ML_BLOCKING on line id 4, which sends a flags row.
m.opening(RX0 + 64, RY0 + 768, RX1 - 64, RY0 + 768, start, start, OPEN, OPEN,
  { special: LINE_SETBLOCKING, arg0: 4, arg1: 0, arg2: 1, playercross: true,
    blocking: false });

// ---------------------------------------------------------------- the filler
// Each block an 11 x 11 grid sharing a line between neighbouring cells, so the counts climb at
// about two lines a sector, and walled in on its perimeter so no walk out of it recurses.
let cellCount = 0;
for (let bj = 0; bj < BLOCKS; ++bj)
{
  for (let bi = 0; bi < BLOCKS; ++bi)
  {
    const i0 = bi * PITCH, j0 = bj * PITCH;
    const cells = [];
    for (let j = 0; j < BLOCK; ++j)
    {
      cells.push([]);
      for (let i = 0; i < BLOCK; ++i)
        cells[j].push(m.sector(CELL_SECTOR));
    }
    cellCount += BLOCK * BLOCK;
    for (let j = 0; j < BLOCK; ++j)
    {
      for (let i = 0; i < BLOCK; ++i)
      {
        const s = cells[j][i];
        const x = at(i0 + i), y = at(j0 + j), xe = at(i0 + i + 1), ye = at(j0 + j + 1);
        // Its west and south edges, so a shared one is written once, then the east and north
        // edges only where the block ends.
        if (i > 0)
          m.opening(x, y, x, ye, s, cells[j][i - 1]);
        else
          m.wall(x, y, x, ye, s, WALL);
        if (j > 0)
          m.opening(x, y, xe, y, cells[j - 1][i], s);
        else
          m.wall(xe, y, x, y, s, WALL);
        if (i === BLOCK - 1)
          m.wall(xe, ye, xe, y, s, WALL);
        if (j === BLOCK - 1)
          m.wall(x, ye, xe, ye, s, WALL);
      }
    }
  }
}

// ---------------------------------------------------------------- the high pass
// Allocated after the filler, so every index here is past 32,767: the door tag 2, the switch that
// opens it, and the sector the point pusher stands in.
const highDoor = m.block(RX1 - 576, RY1 - 512, RX1 - 256, RY1 - 192,
  { heightfloor: 0, heightceiling: 0, ...TEX, id: 2 }, start, () => DOORSIDE, () => DOORSIDE);
const highSwitch = m.wall(RMX + 128, RY0, RMX - 128, RY0, start, SWITCH,
  { special: DOOR_RAISE, arg0: 2, arg1: 16, arg2: 140, playeruse: true,
    repeatspecial: true, id: 4 });
// West of the walk north, so a player reaches the switches without being pushed off course and
// steps into the push only when the measurement asks for it.
const pushSector = m.block(RX0 + 192, RY0 + 1088, RX0 + 576, RY0 + 1472,
  { ...FLAT, id: 3 }, start, () => OPEN, () => OPEN, () => ({ blocking: false }));

m.thing({ x: RX0 + 384, y: RY0 + 1280, angle: 0, type: THING_POINTPUSHER });
m.thing({ x: RMX, y: RY0 + 192, angle: 90, type: 1 });
m.thing({ x: RMX - 320, y: RY0 + 192, angle: 90, type: 2 });

const out = path.join(path.dirname(fileURLToPath(import.meta.url)), "widemap.wad");
fs.writeFileSync(out, wad(mapLumps("MAP36", m)));
console.log(`wrote ${out}: ${m.sectors.length} sectors (${cellCount} filler cells),`
  + ` ${m.lines.length} lines, ${m.sides.length} sides, ${m.vertices.length} vertices`);
console.log(`  low: door sector ${lowDoor}, switch line ${lowSwitch}`);
console.log(`  high: door sector ${highDoor}, switch line ${highSwitch},`
  + ` pusher sector ${pushSector}`);
console.log(`  the start at ${RMX}, ${RY0 + 192}; x ${X0} to ${RX1} (${RX1 - X0} units),`
  + ` y ${X0} to ${-X0} (${-X0 * 2} units)`);
