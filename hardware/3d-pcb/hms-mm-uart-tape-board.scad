// HMS-MM UART "Tape PCB" — 3D printed single-layer substrate + lid
// A REAL, buildable board for the open-source hms-mm firmware (github.com/hms-homelab).
// Two ESP32-C3 SuperMini modules bridged over UART.
//
// Dimensions taken from the fabricated KiCad footprint MODULE_ESP32-C3_SUPERMINI:
//   module body 18.0 (x, across rows) x 22.7 (y, along pins); pin rows +/-7.62 from
//   center (15.24 apart), 2.54 pitch; USB-C 9mm wide on one short end, overhangs 1.3mm.
//
// hms-mm UART pinout (firmware config.h): both boards TX=GPIO2, RX=GPIO3.
//   The board does the crossover (modules 180 deg apart): mule.GPIO2 -> miner.GPIO3
//   and miner.GPIO2 -> mule.GPIO3 run as two PARALLEL diagonals. GPIO2 is a strapping
//   pin but is always TX (idles high) = boot-safe. A shared 3V3 rail powers both from
//   either USB-C. GPIO2/3/3V3/GND all live on the INNER pin row.
//
// Both SuperMinis are pushed FLUSH to the long edges so each USB-C overhangs and a
// cable plugs straight in (the lid wall has a window). All four nets route through
// the interior; GND and 3V3 loop clear of the inner pad columns (>2.5mm wall).
//
// Technique: print, lay copper foil tape over the top, press it into the grooves,
// sand the flat back until only the recessed copper survives, then solder.
// Use a tough filament/resin (not PLA) if you sand.

part = "both";           // "board", "lid", or "both"

// --- Board ---
board_w = 65;
board_h = 30;
board_t = 2.0;

groove_d = 0.8;
sig_w   = 1.5;
gnd_w   = 1.0;
pad_d   = 1.5;
pad_depth = 1.0;

// --- Real SuperMini module ---
mod_w = 18.0;            // body width (x, across the two rows)
mod_l = 22.7;           // body length (y, along the 8 pins)
mod_tol = 0.3;          // pocket clearance per side
module_standoff = 0.4;
row_dx = 7.62;          // pin row offset from module center (x)
usb_w = 11.0;           // USB-C window width (9mm + tolerance)

// Module centers: X staggered, Y set so the USB-C short end is flush to the edge.
// Mule USB -> +Y edge, Miner USB -> -Y edge. body half-length = 11.35.
mule_cx = 17.5;  mule_cy = board_h - mod_l/2;   // 18.65
mine_cx = 47.5;  mine_cy = mod_l/2;             // 11.35

peg_d = 0.7;
peg_h = 2.0;

// Lid
lid_wall = 1.6;
lid_gap  = 0.3;
lid_top_t = 1.5;
lid_h    = 7.0;

// --- Helpers ---
module groove(x1, y1, x2, y2, w) {
    dx = x2 - x1; dy = y2 - y1;
    len = sqrt(dx*dx + dy*dy);
    angle = atan2(dy, dx);
    translate([x1, y1, board_t - groove_d])
        rotate([0, 0, angle]) translate([0, -w/2, 0])
            cube([len, w, groove_d + 0.01]);
}
module pad(x, y) {
    translate([x, y, board_t - pad_depth])
        cylinder(h = pad_depth + 0.01, d = pad_d, $fn = 24);
}
module peg(x, y) {
    translate([x, y, board_t]) cylinder(h = peg_h, d = peg_d, $fn = 16);
}

// --- Pad coords (from footprint local coords; KiCad Y-down -> OpenSCAD Y-up) ---
// Mule rot 0 : board = (cx + xl, cy - yl); inner row xl=+7.62 -> x=25.12
// Miner rot180: board = (cx - xl, cy + yl); inner row xl=+7.62 -> x=39.88
// Inner-row local Y: GPIO2 +2.8, GPIO3 +0.26, 3.3 -4.82, G -7.36, GPIO0 +7.88
mule_tx  = [25.12, mule_cy - 2.80];    // GPIO2  -> 15.85
mule_rx  = [25.12, mule_cy - 0.26];    // GPIO3  -> 18.39
mule_3v3 = [25.12, mule_cy + 4.82];    // 3V3    -> 23.47
mule_gnd = [25.12, mule_cy + 7.36];    // GND    -> 26.01
mine_tx  = [39.88, mine_cy + 2.80];    // GPIO2  -> 14.15
mine_rx  = [39.88, mine_cy + 0.26];    // GPIO3  -> 11.61
mine_3v3 = [39.88, mine_cy - 4.82];    // 3V3    -> 6.53
mine_gnd = [39.88, mine_cy - 7.36];    // GND    -> 3.99

// ============================ BOARD ============================
module pocket(cx, cy, open_top) {
    // snug recess for the module body; open at the USB (edge) end
    pw = mod_w + 2*mod_tol;
    pl = mod_l + 2*mod_tol;
    y0 = open_top ? cy - pl/2 : cy - pl/2 - 0.1;
    ph = open_top ? pl/2 + (board_h - cy) + 0.1 : pl/2 + cy + 0.1;
    translate([cx - pw/2, y0, board_t - module_standoff])
        cube([pw, ph, module_standoff + 0.01]);
}

module board() {
    difference() {
        cube([board_w, board_h, board_t]);

        pocket(mule_cx, mule_cy, true);    // mule, open at +Y edge
        pocket(mine_cx, mine_cy, false);   // miner, open at -Y edge

        // UART signals: two parallel diagonals (mule.TX->miner.RX, miner.TX->mule.RX)
        groove(mule_tx[0], mule_tx[1], mine_rx[0], mine_rx[1], sig_w);
        groove(mine_tx[0], mine_tx[1], mule_rx[0], mule_rx[1], sig_w);

        // GND: mule GND -> right past the miner inner column (x=44, wall ~2.9mm) ->
        //      down under the miner body -> approach miner GND from below the pads
        groove(mule_gnd[0], mule_gnd[1], 44.0, mule_gnd[1], gnd_w);
        groove(44.0, mule_gnd[1], 44.0, mine_gnd[1], gnd_w);
        groove(44.0, mine_gnd[1], mine_gnd[0], mine_gnd[1], gnd_w);

        // 3V3: mule 3V3 -> left of the mule inner column (x=20) -> ALL the way down
        //      below the mule module (y=4.5, clear of GPIO0 and the whole pin column)
        //      -> across the bottom -> up into miner 3V3
        groove(mule_3v3[0], mule_3v3[1], 20.0, mule_3v3[1], gnd_w);
        groove(20.0, mule_3v3[1], 20.0, 4.5, gnd_w);
        groove(20.0, 4.5, 36.0, 4.5, gnd_w);
        groove(36.0, 4.5, 36.0, mine_3v3[1], gnd_w);
        groove(36.0, mine_3v3[1], mine_3v3[0], mine_3v3[1], gnd_w);

        // Solder pads
        pad(mule_tx[0], mule_tx[1]);  pad(mule_rx[0], mule_rx[1]);
        pad(mine_tx[0], mine_tx[1]);  pad(mine_rx[0], mine_rx[1]);
        pad(mule_gnd[0], mule_gnd[1]); pad(mine_gnd[0], mine_gnd[1]);
        pad(mule_3v3[0], mule_3v3[1]); pad(mine_3v3[0], mine_3v3[1]);
        pad(44.0, mule_gnd[1]); pad(44.0, mine_gnd[1]);          // GND corners
        pad(20.0, mule_3v3[1]); pad(20.0, 4.5); pad(36.0, 4.5);  // 3V3 corners
    }

    // Labels on the raised surface (MULE below-left, MINER above-right)
    color("gray") {
        translate([13.0, 4.0, board_t])
            linear_extrude(0.3) text("MULE", size=1.8, font="Liberation Mono", halign="center", valign="center");
        translate([51.0, 28.6, board_t])
            linear_extrude(0.3) text("MINER", size=1.8, font="Liberation Mono", halign="center", valign="center");
    }

    // Alignment pegs on unused pins (GPIO0 inner + GPIO21 outer)
    peg(25.12, mule_cy - 7.88);   // mule GPIO0 -> 10.77
    peg(9.88,  mule_cy - 7.88);   // mule GPIO21
    peg(39.88, mine_cy + 7.88);   // miner GPIO0 -> 19.23
    peg(55.12, mine_cy + 7.88);   // miner GPIO21
}

// ============================= LID =============================
module lid() {
    iw = board_w + 2*lid_gap;  ih = board_h + 2*lid_gap;
    ow = iw + 2*lid_wall;      oh = ih + 2*lid_wall;
    usb_z0 = board_t - 0.5;    usb_h = 5.0;
    difference() {
        translate([-lid_gap - lid_wall, -lid_gap - lid_wall, 0])
            cube([ow, oh, lid_h + lid_top_t]);
        translate([-lid_gap, -lid_gap, -0.1])
            cube([iw, ih, lid_h + 0.1]);
        translate([mule_cx - usb_w/2, board_h + lid_gap - 0.1, usb_z0])
            cube([usb_w, lid_wall + lid_gap + 0.2, usb_h]);          // mule USB window
        translate([mine_cx - usb_w/2, -lid_gap - lid_wall - 0.1, usb_z0])
            cube([usb_w, lid_wall + lid_gap + 0.2, usb_h]);          // miner USB window
    }
}

// ============================ RENDER ===========================
if (part == "board") board();
else if (part == "lid") lid();
else { board(); translate([0, board_h + 12, 0]) lid(); }
