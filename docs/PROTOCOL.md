# Matica XID8300 / XID8100: spool format and network protocol

Notes on how the XID8300 Windows driver (JVC KENWOOD, 2009–2013) talks to the printer, collected for
interoperability: enough detail to drive the printer from Linux. Function addresses refer to the
manufacturer's driver files, which are not part of this project.
Tooling: REA (Ghidra provider). Addresses are Ghidra VAs (image base 0x180000000 unless noted).

Legend: **[obs]** = directly observed in code/bytes · **[inf]** = inferred · **[unk]** = unknown

## 0. Package inventory

| File | Role (from INF) |
|---|---|
| PDR23GDx64.DLL | Printer *DriverFile* (rendering DLL, GDI side) — model "XID8300" |
| PDR23UIx64.DLL | ConfigFile (UI) |
| PDR24PM64.dll | Port monitor DLL, monitor name "XID8300N Port" (MONITOR.INF) |
| PDR24PMUI64.dll, PDR24PM_MG64.DLL, PDR24PM_RT64.DLL, PDR24PM_SV64.EXE, PCP24CT64.dll | Port-monitor helpers (listed in PDR24PM64.cdf) |
| lut/LUT24_B130808A.LUT | "XID8300 Color conversion data" (install.inf) |

### LUT file layout [obs]
- Size 50,331,664 = 16-byte header + 256³ × 3.
- Header: `"LUT24\0_B130808A\0"` (magic + version string).
- Body: 3 bytes per entry, 16,777,216 entries → a full 24-bit RGB → 3-channel cube.
- Entry 0 = `00 00 00`, last entry = `ff ff ff` (so output is in an RGB-like
  sense: white maps to white; driver presumably inverts to YMC afterwards — see §1).

(sections below are filled in as analysis proceeds)

---

## 1. PDR23GDx64.DLL — rendering DLL

Identity: sha256 `344e9226…e074`, PE32+ x64, image base 0x71900000 (Ghidra VAs below),
FileDescription "XID8300 Graphics Driver" 8.01, PDB `PDKTMPGM.pdb` ("GR-PDK Driver CM with UserMode").
REA evidence: overview `ev_98e2f592…`, imports `ev_5dfd25bb…`, exports `ev_e24b9509…`,
decompile-all `ev_639ec64b…` (216 functions), LUT assembly `ev_58a5a180…`.

### 1.1 Architecture [obs]
- A full GDI printer graphics DLL (not Unidrv/GPD). Exports only `DrvEnableDriver`,
  `DrvQueryDriverInfo`, `DrvDisableDriver`, `DllMain`. The DRVFN table at 0x71919430 (34 entries)
  maps: EnablePDEV 0x71911c70, EnableSurface 0x71912ba0, StartDoc 0x7190c440, StartPage 0x7190c2c0,
  SendPage 0x7190c220, EndDoc 0x7190c5c0, StartBanding 0x7190c660, NextBand 0x7190c720,
  Escape 0x71910740, plus the usual Eng* drawing hooks.
- Rendering surface: `EngCreateBitmap(..., BMF_24BPP=5, ...)` / `EngCreateDeviceSurface(...,5)`
  (0x719159e0). Palette `EngCreatePalette(PAL_RGB=4)` when pdev+0x678c==2, else `PAL_BGR=8` (0x71915be0).
  With the default DEVMODE pdev+0x678c = 2 (see 1.9), so **surface bytes are R,G,B in memory** [inf: depends on that default].
- All output goes through `WritePrinter` only (no direct port I/O in this DLL). Buffered writer:
  0x7190ef10 (append) / 0x7190f090 (flush). A per-flush prefix hook (0x71902e60) returns 0 → no prefix.

### 1.2 Spool stream = sequence of records [obs]
Writer 0x71906970 builds every record:

```
struct rec_hdr {            // 20 bytes, little-endian (x64 Windows)
  uint32 type;              // record id (see table)
  uint32 length;            // payload byte count (0 allowed)
  uint32 reserved0;         // always 0
  uint32 reserved1;         // always 0
  uint32 param;             // per-record parameter (only 0x87 uses non-zero)
};
followed by `length` bytes of payload
```
Evidence: GlobalAlloc(0x14); [0]=type, [4]=len, [8]=0, [0xC]=0, [0x10]=param; then payload write.
No compression is applied anywhere in this DLL — planes are written as raw 8-bit bytes
(searched all 216 functions: the only writers are 0x71906970 and its two callees; no RLE/packbits/zlib code).

| type | producer | payload | meaning |
|---|---|---|---|
| 0x01 | 0x71904c80 (StartPage/NextBand, first page only) | 20 bytes | **Job header** (1.3) |
| 0xC0 | 0x7190a7f0 (each page, before planes) | N×4 bytes | printer-setting TLVs (1.4) |
| 0x81 | 0x71907300 | W×H bytes | **Y** plane (8-bit) |
| 0x82 | 0x71907300 | W×H | **M** plane |
| 0x83 | 0x71907300 | W×H | **C** plane |
| 0x84 | 0x71907300 | W×H | **K** plane (resin black) |
| 0x86 | 0x71907300 | W×H | area-fill plane generated from DEVMODE areas (likely peel-off "PO") [inf] |
| 0x87 | 0x71907300 | W×H | **UV / overlay** plane; hdr.param = (mode<<8)\|sub (1.6) |
| 0x88 | 0x71907300 | W×H | "K-secure": K plane scrambled in 60-row strips (0x71909da0) OR'd with the PDR23PD.KS pattern (1.6) |
| 0xD0 | 0x71907300 | 0x404 | error report: uint32 code + WCHAR path[260] (LUT/UV-file load failure) |
| 0x20 / 0x21 | Escape 0x2333 / 0x2332, text `~?4…` / `~?5…` | app data | smart-card data for the **contact** (0x20) / **contactless** (0x21) encoding program (§7) |
| 0x40–0x47 | Escape 0x233C–0x2343, text `~?0…~?9`, `~1%…?` etc. | track bytes | **magnetic-stripe tracks**: 0x41/0x42/0x43 = ISO track 1/2/3 (max 76/37/104 chars), 0x40 = JIS-II (max 69, half-width katakana via SO/SI), 0x44–0x47 other formats; gated by DM+0xee ∈ {0x10,0x11} |
| 0x60 | Escape 0x2454, text `~?@…` | app data | pass-through (purpose unknown) |
| 0x70 | Escape 0x2346, text `~??…` | app data | pass-through, gated by DM+0xee like the mag tracks (purpose unknown) |
| 0xF3 | SendPage 0x71904d90 / EndDoc 0x71904fe0 | 0 | end of card side/card (eject boundary) [inf] |
| 0xF5 | EndDoc 0x71904fe0 | 0 | end of job |

Planes are only emitted if non-empty (0x71905bd0 checks for any non-zero byte); Y/M/C are sent as a
triple if any of the three is non-empty.

### 1.3 Job header (type 0x01, 20 bytes) [obs, field names inf]
Sent once, when page counter (pdev+0x7148) == 1 (0x71904c80):
```
+0  uint32 copies                 (DEVMODE+0x5aa4)
+4  uint8  duplex                 (DM+0xec: 0x10 = 1-sided, 0x20 = 2-sided) | (DM+0xed & 1)
+5  uint8  DM+0xf0                (unknown option)
+6  uint8  magstripe option       (DM+0xee: 0 = off, 0x10 / 0x11 = on; 0x10 vs 0x11 meaning unknown, UI has "Lo-Co"/"Hi-Co")
+7  uint8  smart-card option      (DM+0xef: 0x10 contact, 0x20 contactless, 0x30 both; bit0 = card flag toggle, §7)
+8  uint32 flags: bit0 = DM+0xed bit1; bit12 (0x1000) = DM+0xed bit4 ("duplex 0x21" variant)
+12 uint32 0
+16 uint32 0
```

### 1.4 Settings TLV record (type 0xC0) [obs]
Payload is a list of 4-byte items `{id, 0x02, 0x01, value}`. Items are included only if the
DEVMODE byte != 0xFF (defaults are all 0xFF → normally only sent when DM+0x5a40 == 1):
ids 'H','F','J','K','G','U','V','_','L','M'(5-v),'N'(5-v),']','^','O','P' (DM+0x5a44…0x5a55),
'(' / ')' / '*' (encoder-related, DM+0x5a5c/0x5a5d), 0xF0/0xF1/0xF2, 0xC8 (DM+0x5a90).
Semantics: see §11 (mapped from the printing-preferences UI, PDR23UIx64.DLL).

### 1.5 Geometry, resolution, orientation [obs]
- Resolution table 0x7191b540: 300 × 300 dpi (only entry; PJL "@PJL SET RESOLUTION=300/600" strings exist in
  a template table at 0x7191b5a1/0x7191b661 but are never referenced by code that writes output [inf: bounded grep]).
- Paper table 0x719199d0 ("Standard", id 0x100): 56.0 × 87.5 mm; **plane = 1036 × 664 pixels**
  (DAT_719199e8 = 1036 = width, DAT_719199e4 = 664 = height). Plane size = 687,904 bytes.
  (PDR23PD.KS is exactly 687,904 bytes = one plane.) 1036×664 @300dpi = 87.7 × 56.2 mm (bleed over CR80 85.6×54).
  DM+0xf1 (default 1) forces the GDI page to 0x40c × 0x298 = 1036 × 664 (0x71904890).
- Planes are stored **landscape, row-major, width 1036**: index = y*1036 + x.
- Portrait jobs (pdev+0x7144 == 1, i.e. dmOrientation 1) are rotated while copying from the surface:
  `dst[(664-1-x)*1036 + y] = src(x,y)` (0x71907300 copy loops). Landscape: `dst[y*1036+x]`.
- Optional 180° rotation per side: whole plane reversed (0x71905b80) when
  front: DM+0xf8 == 1, back: DM+0x1158 == 1 (inverted logic for back side in landscape — see code at 0x719083eb).

### 1.6 Colour pipeline and panel split (0x71907300) [obs]
1. Copy surface → three 8-bit planes P0=byte0, P1=byte1, P2=byte2 (init 0xFF = white); K, UV, etc. init 0.
2. Black extraction per DM "K mode" byte (front DM+0xf5, back DM+0x1155; default 0x11):
   - 0x10 / 0x11: pixel exactly (0,0,0) → K=0xFF; 0x10 also whitens RGB there (K only), 0x11 keeps CMY.
   - 0x20 / 0x21: same test on the stored planes (0x20 whitens).
   - 0x40: K derived from luminance (Y = (30R+59G+11B)/100, ≥250 → 255) via 0x71905f50/0x71906780, then inverted.
3. Optional brightness/contrast/gamma/colour-balance (DM+0x22a8==1, 0x71905280).
4. **RGB → CMY by inversion**: each of P0,P1,P2 = 255 − value (0x71905b20).
5. **LUT** (DM+0x22cc==1 and index DM+0x22d0 != −1): see 1.7.
6. Area masks (DM+0x21b4==1, 8 rectangles at DM+0x21b8 stride 0x1c): zero-fill or gradient-fill regions (0x71905c00),
   e.g. to keep mag-stripe / chip areas unprinted.
7. Emit: 0x81 = P2, 0x82 = P1, 0x83 = P0, 0x84 = K. With PAL_RGB surface (P0=R,P1=G,P2=B) this is
   **0x81 = Y, 0x82 = M, 0x83 = C** [inf from default palette; if a PAL_BGR surface were used the 0x81/0x83 roles swap].

Panel option per side (front DM+0xf4, back DM+0x1154; default 3). UI strings at 0x7191a600:
"1. YMC","2. K","3. YMCK","4. YMC+UV","5. K+UV","6. YMCK+UV" ↔ values 1,2,3,0x11,0x12,0x13 (from
validation in Escape 0x238c set-id 0x202/0x203 and the switch in 0x71907300).

| side value | records sent |
|---|---|
| 1 YMC | 0x81 0x82 0x83 |
| 2 K | 0x84 |
| 3 YMCK | 0x81–0x84 |
| 0x11 YMC+UV | 0x81–0x83, 0x87 |
| 0x12 K+UV | 0x84, 0x87 |
| 0x13 YMCK+UV | 0x81–0x84, 0x87 |

Multi-page per side: if K-mode == 0x40 ('@') the K layer comes from an **extra document page**, and if
UV source (DM+0x104 / 0x1164) == 0x40 the UV layer also comes from an extra page. The page→side/layer
mapping is `pageNo mod (pagesFront + pagesBack)` (0x71907300, 0x71904d90). So a YMCK+UV duplex card can
consume up to 6 GDI pages.

UV/overlay plane (0x87) source byte (DM+0x104 front / 0x1164 back, default 0x10):
0x11/0x13 → UV where page pixels are non-white (from a stacked area of the surface); 0x12/0x13 → UV from an
image file (0x71908c10, path table at DM+0x46e4 stride 600); 0x40 → UV from a separate page using a colour key
(DM+0x1150..0x1152). hdr.param = (side_mode<<8) | sub, side_mode from DM+0x114c/0x21ac (1↔2 swapped when rotated),
sub = 0/1/2 from DM+0x4778.

0x88: when DM+0xfc (front) / 0x115c (back) == 1, the K plane is processed by 0x71909da0 and OR'd with the raw
bytes of `%SystemRoot%\System32\SPOOL\DRIVERS\X64\3\PDR23PD.ks` (one 1036×664 plane) and sent as 0x88.

### 1.7 LUT file use [obs — assembly 0x719062c0]
- Path: one of 16 WCHAR[260] paths in the private DEVMODE at DM+0x26d4 + idx*0x208, idx = DM+0x22d0
  (default −1 = disabled; enable flag DM+0x22cc). So the UI points the DEVMODE at the installed LUT file.
- Binary form accepted if file size is 0x3000010 or 0x3000110 and the first 6 bytes are `"LUT24\0"`;
  the next 0x3000000 bytes after the 16-byte header are read (trailing 0x100 bytes of the 0x3000110 variant ignored).
- Applied **after** the RGB→CMY inversion, per pixel:
  ```
  i = (P2<<16) | (P1<<8) | P0          // = (Y<<16)|(M<<8)|C with PAL_RGB surface
  P2 = lut[3*i+0]; P1 = lut[3*i+1]; P0 = lut[3*i+2]
  ```
  i.e. a full 24-bit CMY→CMY ink-space cube, Y-major, 3 bytes out in the same order (Y,M,C).
  Sanity check against LUT24_B130808A: i(0,0,0)→(0,0,0); i(255,255,255)→(255,255,255);
  pure Y→(255,34,0); pure M→(0,217,50); pure C→(107,0,185) — consistent with sRGB-to-process correction.
- Text form: if the first byte is a digit/space/comma/LF and size is 0x300..0x4000, it is parsed as 1024 decimal
  numbers (comma/LF separated, stored as bytes) = four 256-entry 1-D curves applied to P2, P1, P0, K.
- Failure codes returned (sent to printer in a 0xD0 record with the path): 0xEFFFD000 open failed,
  0xEFFFCFFE read failed, 0xEFFFCFFF bad format, 0xEFFFCFFD out of memory.

### 1.8 Duplex [obs]
- DM+0xec: 0x10 simplex, 0x20 duplex (0x21 via escape = duplex + flag 0x10 in DM+0xed). Front side config at DM+0xf4…,
  back side at DM+0x1154… (same layout, stride 0x1060).
- Every GDI page is mapped to (side, layer) as in 1.6; after the last page of the front side and of the
  card, SendPage/EndDoc emit 0xF3 (0x71904d90 / 0x71904fe0), and EndDoc emits 0xF5.
- Escape 0x238c ('G'/'S' get/set) exposes: 0x101 copies (1..999), 0x102 orientation (1/2), 0x103 DM+0xed low nibble,
  0x201 duplex (0,0x10,0x20,0x21), 0x202/0x203 front/back panels, 0x204/0x205 front/back K-mode
  (0x10,0x11,0x20,0x21,0x40), 0x206/0x207 front/back 180° rotate (0/1), 0x301 DM+0xee, 0x302 magstripe, 0x901 port address.

### 1.9 Default private DEVMODE (0x71901bb0) [obs]
copies 1, orientation 2 (landscape), paper 0x100, duplex 0x10, front/back panels 3 (YMCK), K-mode 0x11, UV source 0x10,
DM+0xf1=1, DM+0xf3=8, colour mode DM+0x5ac4=3 and DM+0x5af4=0 → pdev+0x678c=2 → PAL_RGB surface,
LUT disabled (DM+0x22d0 = −1), gamma 1.0 (DM+0x22b4..bc = 10), area masks off.

### 1.10 In-document text escapes (DrvTextOut → 0x71913da0) [obs]
A TextOut string whose first character is `~` is **not printed**; it is converted to a record instead.
Format selected by DM+0x5a61 (1/2/3):
- `~?0` + data → 0x40; `~?1/2/3` → 0x41/0x42/0x43 (format 1); `~?4` → 0x20; `~?5` → 0x21;
  `~?6..9` → 0x44..0x47; `~??` → 0x70; `~?@` → 0x60.
- Format 2: `~1%…?` → 0x41 (≤76) / 0x46 (77–79), `~1\x7f…\x7f` → 0x44, `~2;…?` → 0x42, `~3%…?` → 0x47,
  `~3;…?` → 0x43, `~3\x7f…\x7f` → 0x45 (start/end sentinels checked).
- Format 3: `~1=…`, `~2=…`, `~3=…` → 0x41/0x42/0x43.
Payload = the UTF-16 characters truncated to bytes. A CUPS filter can offer the same via job options.

### 1.11 Encoder control outside the spool stream [obs]
Escape 0x23f0 loads `PCP23CT64.DLL` and calls `CXCMD_ScanPrinter / ScanPrinterNext / TestUnitReady /
ICControl / LoadCard / MoveCard / ReadPosition` (0x71903c50 region) for interactive smart-card encoding.
This bypasses the spool stream (the shipped package contains `PCP24CT64.dll`, not PCP23; not analysed).

### 1.12 Open questions for PDR23GDx64
- [unk] The render surface is allocated **5 × page height** (0x719159e0, StartPage erases height×5). K-modes
  0x10/0x11 and UV source 0x11/0x13 read the 2nd/3rd page-height areas, but no code drawing into those
  areas was found in this DLL (search bounded to the 216 decompiled functions). Until resolved, the safe
  CUPS behaviour is: K plane = pixels that are pure black (mode 0x10 semantics) or no K at all.
- [unk] Exact meaning of record 0xF3 (per side vs per card) — inferred from where it is emitted.
- [unk] Meanings of the 0xC0 TLV ids, of DM+0xf0, DM+0xed bits, and of 0x86 (peel-off is an inference from
  the "YMCK-PO" ribbon string and the gradient area fill).
- [unk] Whether the printer requires 0x01 before 0xC0 on later pages, or accepts planes in a different order;
  only the driver's order (0x01, 0xC0, 0x81, 0x82, 0x83, 0x84, [0x88], [0x87], [0x86], 0xF3…, 0xF5) is known.
- [inf] Plane colour roles (0x81=Y, 0x83=C) rely on the default PAL_RGB surface; verify with a test print
  (e.g. send a Y-only plane).
- [unk] No byte-level capture of a real spool file was available to confirm the whole stream.

---

## 2. PDR24PM64.dll — "XID8300N Port" TCP/IP port monitor

Identity: sha256 `5663fe64…b057`, PE32+ x64, image base 0x50400000, FileDescription "XID8300 TCP/IP Port Monitor" 8.01,
PDB `PDR24PM64.pdb` (framework name "GR-PMK"). REA evidence: imports `ev_d453173a…`, exports `ev_9e9891b9…`,
decompile-all `ev_e89dbf8c…` (186 functions), strings `ev_44647388…`.

### 2.1 Key observation: no socket code in this DLL [obs]
Imports contain **no Winsock**. All printer I/O goes through **PCP24CT64.dll** (imported by name):
`P_Connect, P_SendPrintJob, P_CheckGoAhead, P_KeepAlive, P_FlushPropertyReq, P_FlushStatusReq, P_FlushSettingReq,
P_FlushSettingValue, P_ImageOut, P_PrintOnFilm, P_PrintOnCard, P_SecurityPrint, P_LoadCard, P_MoveCard,
P_ReadPosition, P_RezeroUnit, P_TestUnitReady, P_WriteMG, P_ReadMG, P_ICControl, P_CheckReply, P_Close`
plus builders `U_Set1stPrinterInfo/U_SetNextPrinterInfo/U_Set1stSettingValue/U_SetNextSettingValue/U_InitializeSettingValue`.
Status pop-ups go to `PDR24PM_RT64.DLL` (`RtouOpenSessionByAddrW/RtouSendMessage/RtouCloseSession`) and WTSSendMessageW.
Optional plug-ins loaded by name if present (not shipped): `PDR24EX.DLL!UserPort`, `PDR24ST.DLL!PrinterStatus`,
`PDR24IC0.DLL!Encode` (contact IC), `PDR24IC1.DLL!Encode` (contactless IC), `PDR24MG.DLL!MagControl`.
→ The byte-level wire format lives in PCP24CT64.dll (section 3).

### 2.2 MONITOR2 table (0x504149f0) [obs]
OpenPort 0x50403964, StartDocPort 0x50403a00, WritePort 0x50403cc8, ReadPort 0x50403c20, EndDocPort 0x50403ba8,
ClosePort 0x504039b8, AddPortEx 0x5040383c, Xcv* 0x50403628/0x504036fc/0x504037c8, SendRecvBidiDataFromPort 0x50403da8.
Port kinds (port+0x2f0): 0 = "NULL Port" (discard), 1 = "FILE Port" (WriteFile), 3 = "CX Printer Port for TCP/IP".
Per-port registry values under `…\Monitors\XID8300N Port\Ports\<port>`: `IPaddr`, `IPv4v6`, `ScsiId`,
`Unit_Turnover/Remedy/ICConless/ICContact/MGISO/MGJIS/Lock/Laminate`.

### 2.3 Sequence on the wire (call level) [obs]
**StartDocPort** (0x504041ac → 0x504090e8):
1. `GetUserNameW` → user name (fallback "No name"); document name truncated to 32 WCHAR.
2. `P_Connect(&h, 0, &status, addrType, addr16, …, timeout=2000 ms)` — address = 16 bytes from port config
   (IPv4 in first 4 bytes, or IPv6), type byte from port+0x4fa.
3. `P_SendPrintJob(h, &status, userName, docName)`.
4. Poll `P_CheckGoAhead(h, …)` every 100 ms: 0 = wait, 1 = go, 2 = keep waiting while sending `P_KeepAlive(h)`;
   −1 with status 0x2d300 / 0x2d400 / 0x2d500 → busy-type codes 2/4/5 (retry via user dialog), other → error.
5. `U_Set1stPrinterInfo(0x0d)` + `P_FlushPropertyReq` + `P_CheckReply` — expects reply type 2, length 0x0c.
6. Open two temp files `%TEMP%\DR24PM_nnn.TMP` (front side / back side buffers).

**WritePort** (0x50407aa4): parses the spool records from section 1.2:
- first record must be type 0x01 (job header, 40 bytes total); it is kept and copied at the start of the front-side file.
- every record is appended to the *front* temp file until 0xF3; if job header byte+4 & 0xFE == 0x20 (duplex),
  records after the first 0xF3 go to the *back* temp file until the next 0xF3/0xF5.
- per side a mask is collected: 0x81/0x82/0x83 → bit0 (YMC), 0x84 → bit1 (K), 0x87 → bit2 (UV), 0x86 → bit3;
  encoder mask: 0x20 → 1, 0x21 → 2, 0x40 → 4, 0x41–0x47 → 8, 0x70 → 0xC.
- at a card boundary, the card is sent `copies` times (job header +0) by 0x504047f8, then new temp files are opened.

**Per card** (0x504047f8, simplified; error/retry paths omitted):
1. `P_FlushStatusReq` (info id 0x0d) and `P_FlushPropertyReq` with printer-info ids 0x32,0x33,0x36–0x3b → printer state.
2. Read the side file(s) back into per-type buffers (0x5040889c).
3. Optional encoder steps (mag `P_WriteMG/P_ReadMG` 0x50409a74/0x50409da8, IC `P_ICControl` 0x5040939c,
   with `P_LoadCard/P_MoveCard/P_ReadPosition/P_RezeroUnit` positioning 0x504059a0).
4. `P_FlushSettingValue` with the 0xC0 TLVs (ids F,G,H,I,J,K,L,M,N,O,P,U,V,W,[,(,),*,0xC8,],^,_) (0x5040c014).
   TLV ids 0xF0/0xF1/0xF2 are consumed by the monitor itself (port+0x1320..0x1322).
5. For each side: `P_ImageOut` once per plane, then `P_PrintOnFilm(mask)` (0x504065e4):
   | spool record | ImageOut plane id |
   |---|---|
   | 0x81 (Y) | 1 |
   | 0x82 (M) | 2 |
   | 0x83 (C) | 4 |
   | 0x84 (K) | 8 |
   | 0x88 (K-secure) | 8 (followed by `P_SecurityPrint(2)` instead of PrintOnFilm) |
   | 0x87 (UV) | 0x40 |
   | 0x86 | 0x20 |
   Payload = plane bytes **without** the 20-byte record header, length = rec.len (687,904 for a full plane).
   If a side has **no planes at all**, the monitor sends an all-zero K plane (alloc 0xA7F34 = 687,904+20)
   (corrected after re-reading 0x50405cf0: the blank K is only used when nothing else is present).
   Masks used: 0xF / 0xB / 3 (YMC+K), 4 (UV), 0x200 (K-secure).
6. `P_PrintOnCard(mode)` — mode 0 or 5 (5 used when param==1, i.e. the duplex/turnover path) transfers film to card.
7. `P_TestUnitReady` polling between steps (0x50406798/0x50406828), `P_MoveCard(4,…)` to eject (0x50405bd8).
Every request increments a global 32-bit tag (DAT_50414920) and is followed by `P_CheckReply(h, …, timeout=60000 ms)`.

**EndDocPort** (0x50403f60): `P_Close(h)`, close/delete temp files, `SetJobW(JOB_CONTROL_SENT_TO_PRINTER)`.

### 2.4 Status / error codes [obs]
P_* return −1 with a 24-bit status: 0x2d300, 0x2d400, 0x2d500 are treated as "busy"-class retryable states;
others become 0x01xxxxxx (printer status), −2 → 0x03xxxxxx (comm), other → 0x04xxxxxx. Human-readable messages for
these are in the string table (e.g. "Retransfer film has run out.", "Ink ribbon specification is incorrect.").
The 0xD0 spool record is turned into "The specified LUT/UV file is not found/cannot be opened (%s)".

---

## 3. PCP24CT64.dll — the TCP wire protocol used by the port monitor

Identity: sha256 `3e86af3d…8437`, PE32+ x64 (MFC), image base 0x180000000. 42 exports (`ev_b886ab67…`), imports WS2_32 by
ordinal (socket, connect, send, sendto, recv, select, ioctlsocket, bind, htons, WSAStartup…) (`ev_24d56d6c…`).
Decompiled exported range 0x180001000–0x180008000 (`ev_4773e809…`). All multi-byte fields below are **big-endian**.

### 3.1 Transport [obs — P_Connect 0x1800012f0]
- `P_Connect(h, kind, …)`: kind 0 → **TCP port 50710 (0xC616)**; kind 2 → TCP 50712; kind 3 → TCP 50713;
  kind 1 → UDP 50711 (0xC617, discovery/list). IPv4 (`AF_INET`) or IPv6 (`AF_INET6`) per the port's `IPv4v6` flag.
  The port monitor uses kind 0.
- After connect the client waits (select, 30 s) for a **0x78-byte greeting** from the printer:
  `F3 00 0T …` with T = 2 → OK, T = 1 → refused/busy; bytes 0x0C..0x0F stored as a session value; bytes 0x10..0x77
  (0x68 bytes) = printer information returned to the caller.
- `P_Close` = closesocket. Each receive is a single `recv` of ≤ 0x200 bytes (no reassembly) after `select`.

### 3.2 Common frame header [obs]
```
off  size  field
0    1     kind: F0 = command (host→printer), F1 = command reply, F2 = session/control (host→printer),
                 F3 = session/control (printer→host)
1    2     message code (BE)
3    1     0
4    4     length of everything after byte 8, in 32-bit words (BE)
8    4     host→printer: flags (byte 0x0B = 0x02 when the API's last arg is 1; 0x04 on some info requests)
           printer→host: status / sense code (BE; e.g. 0x0002D300)
12   4     tag (BE) — per-request sequence number chosen by the host, echoed in the reply
16   …     body, padded with zeros to a multiple of 4
```

### 3.3 Session messages (F2 / F3) [obs]
| Message | Bytes |
|---|---|
| Start job (`P_SendPrintJob`, 0x7C bytes) | `F2 00 03 00  00 00 00 1D  00 00 00 02  99 99 99 99  00 00 00 00` + `yy mm dd hh mi ss 00 00` (struct tm: year−1900, month 0-11) + user name UTF-16LE[16] (32 B) + document name UTF-16LE[32] (64 B) |
| Keep-alive (`P_KeepAlive`) | `F2 00 01 00 00 00 00 00` |
| Go-ahead (printer → host, 16 B) | `F3 00 04 …` = go; `F3 00 03 …` = still waiting (host sends keep-alive) |

### 3.4 Command messages (F0) [obs]
Code 0x0001 = card/print control, 20-byte frame `F0 00 01 00 00 00 00 03 00 00 00 FF tag(4) SS LL P1 P2`
(PrintOnCard is 24 bytes, length 4). SS = sub-command, LL = 0x02 (bytes that follow), FF = 0x02 when "last arg = 1".

| API | SS | P1 | P2 / extra |
|---|---|---|---|
| P_TestUnitReady | 01 | 00 | 00 |
| P_ReadPosition | 02 | 00 | 00 |
| P_RezeroUnit | 03 | arg & 0x1F | 00 |
| P_LoadCard | 04 | flags: 0x10 / 0x80 / 0x90 | position 0..5 |
| P_MoveCard | 05 | flags as LoadCard | destination 0..5 (monitor uses 4 to eject) |
| P_PrintOnFilm | 06 | 0x20 if side-flag; \|1 or \|2 from mask bits 0x30 | plane bits: 0x07 = Y\|M\|C (mask&1), 0x08 = K (mask&2), 0x40 = UV (mask&4), 0x20 = 0x86-plane (mask&8) |
| P_PrintOnCard | 07 (LL=06) | flags 0x00/0x10/0x20/0x30 | mode 0 or 5, then one byte arg, 3×00 |
| P_WriteMG | 09 | variable | track data (not fully decoded) |
| P_ICControl | 0A | ((b&1)<<2 \| (a&1))<<4 | 00 |
| P_SecurityPrint | 0B | 0x20 if side-flag | 08 |

Other codes: 0x0002 = **ImageOut**, 0x0003 = **LUTOut** (exported, *not* used by the port monitor),
0x0004/0x000A = FlushSettingReq (read settings), 0x0005 = FlushPropertyReq (printer info),
0x0006 = FlushStatusReq, 0x0008 = FlushSettingValue (write settings).

**ImageOut (code 2)** — one plane per message:
```
F0 00 02 00 | words=(total/4)-2 | 00 00 00 00 | tag
[0x10] plane id: 01=Y 02=M 04=C 08=K 20=(0x86 plane) 40=UV
[0x11] 0x20 if side-flag (second/back image), else 0
[0x12..0x13] 00 00
[0x14..0x17] total − 0x18  (BE)
[0x18..0x1B] data length    (BE)  — 687,904 for a 1036×664 plane
[0x1C…]      raw 8-bit plane bytes, then zero pad to 4
```
total = (len + 0x1F) & ~3. Planes are sent **uncompressed**.

**Info / setting lists** (codes 4,5,6,8): header as 3.2, then items. Printer-info requests use 4-byte items
`{id,0,0,0}` (`U_Set1stPrinterInfo/U_SetNextPrinterInfo`). Setting values use TLVs
`{id, padded_len−2, data_len, data…}` padded to 4 (`U_Set1stSettingValue/U_SetNextSettingValue`) — exactly the
4-byte `{id,02,01,value}` items the graphics driver puts in spool record 0xC0, which the monitor forwards.

### 3.5 Replies (P_CheckReply 0x180003a50) [obs]
Reply = F1 or F3 frame, ≥ 16 bytes. status = bytes 8..11, tag = 12..15, body from 16.
F1 code 2 → error (return −1, status = sense such as 0x2D300/0x2D400/0x2D500); other F1 codes: 4 → kind 1,
5 → kind 2 (property data), 6 → kind 3 (status data), 3 → magstripe read data (kinds 4/5), 7 → 8, 8 → 9.
F3 code 6 → 6, F3 code 5 → 7. The monitor waits up to 60 s for each reply.

### 3.6 Full job as seen on the wire (simplex YMCK card, default options) [obs+inf]
```
TCP connect printer:50710
← F3 00 02 … (0x78-byte greeting)
→ F2 00 03 StartJob(user, document, timestamp)
← F3 00 03 (wait)   → F2 00 01 keep-alive   … repeat
← F3 00 04 (go)
→ F0 00 05 FlushPropertyReq [0x0D]                    ← F1 reply
  (per card, × copies)
→ F0 00 06 FlushStatusReq [0x0D]                      ← F1 00 06 …
→ F0 00 05 FlushPropertyReq [0x32,0x33,0x36..0x3B]    ← F1 00 05 …
→ F0 00 08 FlushSettingValue (TLVs from record 0xC0, may be empty)
→ F0 00 01 ReadPosition; LoadCard/MoveCard to position 0 if needed
→ F0 00 01 TestUnitReady (polled every 20–100 ms until ready)
→ F0 00 02 ImageOut Y, M, C, K [, 0x86] [, UV]        (each answered by an F1 reply)
→ F0 00 01 PrintOnFilm (mask 0xF)
→ TestUnitReady polling
→ F0 00 01 PrintOnCard (mode 5 for simplex; mode 0 + flag 0x10 for the first side of a duplex card)
(→ F0 00 01 ReadPosition / MoveCard(4) eject — seen on the cleanup/error path 0x50405bd8; on success
   PrintOnCard mode 5 is assumed to finish the card [inf])
TCP close
```
Duplex: the back side is sent with ImageOut byte 0x11 = 0x20 and PrintOnFilm P1 bit 0x20 (side-flag = 1), followed
by a second PrintOnCard. When UV overlaps the colour/K image (0x50406aac, 2-pixel dilation test) the UV plane is printed
in a separate ImageOut(4)+PrintOnFilm(4) pass. K-secure (0x88) is sent as plane 8 followed by `PrintOnFilm(3)`,
`SecurityPrint`, then UV.

Duplex order [obs, 0x504047f8 state bytes 0x0c–0x1a]: front pass (ImageOut side-flag 0 → PrintOnFilm → TUR →
PrintOnCard mode 0, flags 0x10), then back pass (ImageOut side-flag 1 → PrintOnFilm side-flag 1 → TUR → PrintOnCard
mode 5, or mode 0/flags 0 when UV needs its own pass). Job-header flag 0x1000 (DM+0xed bit4) reverses the order (back first).

---

## 4. Practical guidance for the Linux side

Two layers are cleanly separable:
1. **Raster layer (CUPS filter)** — render the page at 300 dpi to a 1036×664 landscape RGB raster (rotate portrait
   input 90° as in 1.5), extract K, invert RGB→CMY (255−v), optionally apply the LUT cube
   (`i=(Y<<16)|(M<<8)|C; Y,M,C = lut[16+3i .. 16+3i+2]`), and produce 8-bit Y, M, C, K (and optional UV) planes.
2. **Transport layer (CUPS backend)** — implement section 3: TCP 50710, wait for the F3 greeting, StartJob, wait for
   go-ahead (send keep-alives), then per card: status/property queries, optional settings, card load, ImageOut ×N,
   PrintOnFilm, PrintOnCard, close. Every F0 command must be answered (F1) before the next; reuse an incrementing tag.

The Windows split (driver → spool records → port monitor → TCP) can be mirrored: have the filter emit the
spool-record format of 1.2 (it is simple and self-describing) and have the backend translate it exactly like
PDR24PM64 does. This also makes captures from a Windows PC directly comparable.

## 5. Consolidated list of unknowns / things to verify on hardware

| # | Question | Why unknown | Suggested check |
|---|---|---|---|
| U1 | Plane colour roles 0x81/ImageOut 1 = Y, 0x83/ImageOut 4 = C | depends on the GDI palette default (PAL_RGB) | print a single-plane test card |
| U2 | How the 2nd–5th page-height areas of the GDI surface are drawn (K-mode 0x10/0x11, UV 0x11/0x13 sources) | no drawing code found in the 216 decompiled functions | ignore; derive K from pure black |
| U3 | Exact meaning of PrintOnCard mode 0 vs 5, flags 0x10/0x20/0x30, and the 1-byte arg at 0x14 | only call sites seen, monitor passes fixed values | capture Windows traffic (Wireshark, tcp.port==50710) |
| U4 | Meaning of header byte 0x0B = 0x02 / 0x04 | set from a boolean API argument | capture |
| U5 | Contents of the 0x78-byte greeting and of property/status replies (ids 0x0D, 0x32, 0x33, 0x36–0x3B) | parsed only partially by the monitor | capture / probe |
| U6 | Status codes 0x2D300 / 0x2D400 / 0x2D500 and the full error table mapping | table in PCP24CT64 (0x180041000) not decoded | decode table / capture |
| U7 | Meaning of the 0xC0 setting ids (F,G,H,…,0xC8) | values are opaque bytes from the UI | UI DLL PDR23UIx64.DLL not analysed |
| U8 | Magnetic-stripe (P_WriteMG, code 1/sub 09) and IC encoding framing details | not needed for printing; only partly decoded | analyse P_WriteMG 0x180003080 if needed |
| U9 | Whether the printer accepts a LUT upload (`P_LUTOut`, code 3) instead of host-side LUT | exported but unused by the monitor | test carefully |
| U10 | 0x86 plane purpose (peel-off?) and 0x88 K-secure semantics on the printer | inferred from strings/UI | capture with those options |
| U11 | USB transport ("XID8300U Port", ScsiId registry value) | USB monitor not shipped in this package | — |
| U12 | Mag-stripe tracks: Lo-Co/Hi-Co selection (DM+0xee 0x10 vs 0x11) | only gating observed | — |

Nothing here was observed at runtime: all statements are from static analysis (Ghidra 12.1.4 via REA) of the shipped
binaries; no network capture or hardware test was available.

### 2.5 Wrapper return conventions (verified from assembly, `ev_6237ee09…`, `ev_47e12ef4…`, `ev_38291885…`) [obs]
- TestUnitReady wrapper 0x5040ac64: OK → 0; error reply with status 0x2D300 → 2, 0x2D400 → 4, 0x2D500 → 5
  (positive = keep polling); other statuses → −(status|0x01000000).
- Two polling helpers stop on different busy states: 0x50406828 accepts 0 or 4, 0x50406798 accepts 0 or 2; the card
  flow calls them in that order after every PrintOnFilm and before PrintOnCard.
- ReadPosition wrapper 0x50406954: reply must be F1 code 3 with body[0]=1 and 4 bytes; body[2] bit2 set → no card
  (−1, or −2 if bit0 also set); otherwise position = body[3]; communication/other errors → −3.
- Initial property request in StartDocPort uses `P_FlushPropertyReq(..., 1)` (header byte 0x0B = 4) and expects
  reply code 5 with a 12-byte body.

---

## 6. Magnetic stripe encoding (added for the CUPS backend)

### 6.1 Wire format — `P_WriteMG` (PCP24CT64 0x180003080) [obs]
Command code 1, sub-command **0x09**, variable length:
```
F0 00 01 00 | words | 00 00 00 00 | tag
[0x10] 09
[0x11] (allocated length − 0x12)        allocated = (0x14 + 2·n + Σlen + 3) & ~3
[0x12] 00
[0x13] n = number of track blocks (1..3)
then n blocks: { format, len, data[len] } in order track-1 slot, track-2 slot, track-3 slot; zero pad to 4
```
| format | track | char encoding on the wire | max chars (truncated by the DLL) |
|---|---|---|---|
| 0x07 | JIS-II | `c & 0x7F` | 69 |
| 0x16 | 1 (ISO) | `(c − 0x20) & 0x3F` | 76 |
| 0x17 | 1, 7-bit | `c & 0x7F` | 69 |
| 0x18 | 1, 6-bit long | `(c − 0x20) & 0x3F` | 79 |
| 0x24 | 2 (ISO) | `c & 0x0F` | 37 |
| 0x34 | 3 (ISO) | `c & 0x0F` | 104 |
| 0x36 | 3, 6-bit | `(c − 0x20) & 0x3F` | 79 |
| 0x37 | 3, 7-bit | `c & 0x7F` | 69 |
Data excludes start/end sentinels: the driver's `~1%…?` text form strips them (0x71913da0) before the record is written.

`P_ReadMG` = sub-command 0x08 (LL 06, bytes 0x13..0x15 = the three format codes); reply F1 code 3 with
body[0] = 8 carries the decoded tracks. The normal print path does not read back.

### 6.2 Spool record → format (monitor 0x50409b5c / 0x5040b968) [obs]
0x40 → 0x07 (sent alone, first, via 0x5040b680); 0x41 → 0x16, 0x44 → 0x17, 0x46 → 0x18 (later record wins);
0x42 → 0x24; 0x43 → 0x34, 0x45 → 0x37, 0x47 → 0x36. ISO tracks go in **one** WriteMG.
Records 0x70 / the `PDR24MG.DLL!MagControl` plug-in path (0x50409da8) are for interactive read/write and are not
part of normal printing.

### 6.3 Card flow with encoding (0x504047f8, 0x50409a74, 0x5040939c, 0x504059a0) [obs]
Applies when job-header byte 6 (DM+0xee) ≠ 0 and the front side carries 0x40–0x47 records:
1. Initial card feed goes to **position 3** (magnetic encoder) instead of 0: ReadPosition → LoadCard(3)/MoveCard(3).
2. Front side film is imaged and printed as usual (ImageOut…, PrintOnFilm), then TestUnitReady until 0/0x2D400.
3. Encode: (position 3 confirmed), WriteMG JIS-II if present, WriteMG ISO tracks; busy → retry every 100 ms.
4. If job-header byte 6 has bit0 set (value 0x11) the LoadCard/MoveCard flag is toggled.
5. Card back to **position 0** (MoveCard(0, flag)), then TestUnitReady (0x2D400), TestUnitReady (0x2D300), PrintOnCard.
6. On an encoding error the monitor ejects the card (MoveCard 4).
If settings TLV 0xF2 is non-zero, steps 3–5 happen before the film is printed instead.
[unk] What flag 0x10 on LoadCard/MoveCard does physically (inferred: card turned over), and whether DM+0xee
0x10/0x11 correspond to the UI's "Lo-Co"/"Hi-Co" strings. Coercivity is not sent in any frame seen here; it may be
one of the 0xC0 setting ids '(' ')' '*' (sent only when DM+0xee ≠ 0).

---

## 7. Smart-card (IC) encoding (added for the CUPS backend)

### 7.1 Architecture [obs]
The printer does **not** receive chip data. PCP24CT64 has no APDU/transmit function; the port monitor only
**positions the card and engages the encoding station**, then calls an external, vendor/user-supplied plug-in that
talks to the chip itself (typically through a PC/SC reader built into the printer):
- contactless: `PDR24IC0.DLL!Encode` (error table entry 0xEFFFFCFF "Non-contact IC encoding program is not found")
- contact:     `PDR24IC1.DLL!Encode` (0xEFFFFDFF "Contact IC encoding program is not found")
Neither DLL is in the driver package.
`Encode(sessionHandle, int *out, const uint8 encoderInfo[10], const uint8 *data)` — `data` is the payload of the
spool record; its length is **not** passed (the plug-in must know its own format).

### 7.2 Spool records and job header [obs]
| record | from (driver) | station | job-header byte 7 bit | LoadCard position | ICControl unit |
|---|---|---|---|---|---|
| 0x20 | Escape 0x2333, text `~?4…` | **contact** | 0x10 | 1 | 0 |
| 0x21 | Escape 0x2332, text `~?5…` | **contactless** | 0x20 | 2 | 1 |
(corrects §1.2/§1.3, which only said "unit A / unit B"). Byte-7 bit0 (values 0x11/0x21/0x31) toggles the
LoadCard/MoveCard flag after IC encoding, like byte-6 bit0 does for the magnetic stripe.

### 7.3 Commands [obs]
- `P_ICControl` (0x1800034f0): code 1, sub-command **0x0A**, header byte 0x0B = 0,
  P1 = ((release & 1) << 2 | (unit & 1)) << 4 → contact engage 0x00, contact release 0x40,
  contactless engage 0x10, contactless release 0x50 (monitor wrapper 0x5040bf00 passes (unit, release)).
- Encoder info: `P_FlushSettingReq` (code **0x0A**, byte 0x0B = 4) with info item `{01,0,0,0}`; reply F1 code 4
  whose TLV body (`{id, entlen, len, data}`, parser 0x504090a8) item 1 supplies the 10 bytes passed to `Encode`.
  Failure is ignored (buffer stays zero).

### 7.4 Flow (0x5040939c, 0x50409a74) [obs]
Runs at the same point as magnetic encoding (after the first film pass, §6.3), in this order:
1. contactless (if byte-7 bit 0x20 and record 0x21 present): card → position 2 (LoadCard/MoveCard + TestUnitReady),
   ICControl(1, engage), encoder info, `PDR24IC0!Encode`, ICControl(1, release) — release is sent even if Encode failed;
2. contact (bit 0x10 and record 0x20): same with position 1, unit 0, `PDR24IC1!Encode`;
3. if byte 7 has bit0 set: toggle the card flag;
4. magnetic stripe (§6.3) at position 3;
then the caller moves the card to position 0 and transfers the film.
The initial card feed (0x50409a74) goes to the first station needed: 2, else 1, else 3, else 0.
Plug-in results: 0 = OK; **0x1001** = card is ejected and the job stops without the error dialog; **0x1002** = card
ejected and the job is deleted (`SetJobW(…, 5)` in 0x504085d4); anything else → 0xEFFFFDFD/0xEFFFFCFD
"error during (non-)contact IC encoding data writing", error dialog, card ejected. A missing plug-in gives the
"program is not found" error before the station is engaged.

---

## 8. K-secure (0x88) and the peel-off plane (0x86) (added for the CUPS driver)

The behaviour below was confirmed by running the Windows driver's routines under an emulator on random inputs.
The print-area behaviour (8.2) is implemented in `src/xid8300_areas.h` and checked against the recorded
results (`tests/golden.sha256`); K-secure (8.1) is described for reference but not implemented.

### 8.1 K-secure plane (PDR23GDx64 0x71909da0, called from 0x71907300) [obs, emulation-verified]
Enabled per side by DM+0xfc (front) / DM+0x115c (back), only on sides with a K panel, and only sent if K is non-empty.
Input = the final K plane (after 180° rotation and area cut-outs). Strips of 60 dots: rows for portrait jobs
(11 strips), columns for landscape jobs (17 strips); the leftover 4/16 dots are untouched.
1. In each strip, find runs of lines (columns resp. rows) that contain K. A run closes at the first empty line;
   at the last line it closes *without* including that line (quirk). Runs of ≥2 lines are mirrored into a
   scratch plane; K |= scratch.
2. Same scan on the new K; runs longer than 3 lines are split into quarters (n = len>>2) and the two halves
   swapped (Q0,Q1 → +2n; Q2,Q3 → −2n); K |= scratch.
3. Result OR'ed with the 687,904-byte pattern file `PDR23PD.KS` (silently skipped if the file is missing) → record 0x88.
Purpose [inf]: printed with `SecurityPrint` on the K panel, it scrambles what the used ribbon would reveal.

### 8.2 Print areas and the 0x86 plane (0x71905c00) [obs, emulation-verified]
DM+0x21b4 enables up to 8 areas at DM+0x21b8, stride 0x1c: `{enabled, x, y, w, h, type, side(1/2)}` in plane
coordinates **after** rotation. Applied to Y/M/C/K and UV:
- type 2 (no-print): exact rectangle set to 0 (not clipped);
- any other type: rectangle grown by 4 dots left/right, 6 dots up/down (clipped), set to 0.
0x86 plane (only on sides with a K panel): for type-1 areas, rectangle shifted up 8 dots: 5 rows of DM+0x229c
(default 0xE1), then a ramp `start − (start−end)·k/(rows−5)` (integer division, floor at end) from DM+0x22a0 to
DM+0x22a4 (defaults 0xA0/0xA0). Inference: type 1 = peel-off area (e.g. chip or signature panel), the 0x86
plane drives a peel-off ("PO") ribbon panel (UI strings "YMCK-PO", "1. Peeloff").

### 8.3 Monitor handling (0x504047f8, 0x50405cf0, 0x504065e4, 0x50406aac) [obs]
- 0x88 is sent with ImageOut **plane id 0x08** and followed by `P_SecurityPrint` (sub-command 0x0B, header byte
  0x0B = 2, P1 = 0x20 on the second side, P2 = 0x08) instead of PrintOnFilm.
- 0x86 is sent with plane id 0x20; PrintOnFilm P2 bit 0x20.
- Film passes per transfer (TestUnitReady until 0/0x2D300 between passes):
  | case | first transfer | second transfer |
  |---|---|---|
  | normal | [Y M C K UV 0x86] | — |
  | UV + other panels | [Y M C K 0x86] | [UV] |
  | K-secure | [Y M C K], [K-secure → SecurityPrint], [UV] | — |
  | K-secure + UV + others | [Y M C K], [K-secure → SecurityPrint] | [UV] |
- 0x86 together with UV, or 0x86 together with 0x88, is rejected: 0xEFFFEFFE "Ink ribbon specification is incorrect".

---

## 9. USB connection (what is known)

- Only the **network** port monitor ("XID8300N Port", PDR24PM64.dll + PCP24CT64.dll) is in this driver package.
  The rendering DLL also reads the registry key `…\Monitors\XID8300U Port\Ports` (0x71901588) and, first of all,
  a value `ScsiId` (0x71906a90); its address report for Escape 0x238c id 0x901 (0x71906cd0) has three types:
  1 = SCSI ID (4+4 bits), 2 = IPv4, 3 = IPv6. So a USB port monitor ("XID8300U Port") exists in Matica's driver
  family, but **it is not in this package**. [obs]
- No file in the package contains USB device names, `DeviceIoControl`/SCSI pass-through, or USB IDs. [obs, bounded
  string search of all 10 binaries]
- [inf] The printer's USB connection is probably presented to the host as a SCSI-type device (addressed by a SCSI ID),
  matching the SCSI-style command names of the network protocol (TestUnitReady, RezeroUnit, "Unit Attention"). It
  is therefore unlikely to appear on Linux as an ordinary USB printer (`/dev/usb/lp*`, CUPS `usb://`).
- [unk] USB vendor/product IDs, interface class, and how commands and image data are framed over USB.
  Needed to support it: `lsusb -v` of the printer and, ideally, a USBPcap/Wireshark capture of the Windows driver
  printing one card over USB.

---

## 10. Results on a real XID8100

- The test printer is a **Matica XID8100**, normally printed from Windows with the **XID8300 driver**, i.e. it
  uses the same protocol.
- First Linux test: the job stopped with "Settings failed, printer status 0x02DD00".
  Everything before that step was accepted by the real printer: TCP connect (port 50710), greeting, StartJob,
  go-ahead, the initial property request, the per-card status request and the property request. **This confirms
  the transport, framing, session handshake and info requests of §3 on real hardware.**
- 0x02DD00 = "Operation panel is currently in use, or the printer is in the transport mode." (monitor table
  0x50410420). Status values have SCSI sense layout: sense key (0x02 = not ready), ASC 0xDD, ASCQ 0x00. The monitor
  classes it with the busy states (offers "Retry"), so it is a printer state, not a protocol rejection.
- Status table (sense key 2 "not ready" unless noted): D0 no card, D1 door open, D3/D4/D5 busy, DA preheating,
  DB initializing, DC offline test/cleaning, DD panel in use/transport mode, FD power saving (monitor sends
  `RezeroUnit` with argument 1, 0x5040ad70, then retries), FE password not entered; key 3: 9x card jams/loading,
  A1 film, AD/AE magnetic write/read, B0/B1 ribbon; key 5: 27 ribbon spec, 2B mag data, 2C/2D/2E option not
  installed; key 6: 28/29 initializing; 0x42A200 film out, 0x42B200 ribbon out.
- With waiting on not-ready states added, **a card printed** (portrait design, colour + K). So the settings command, card load,
  ImageOut, PrintOnFilm and PrintOnCard sequence of §3 works on the XID8100 with 1036×664 planes.
- The card had a white strip (~2–2.5 mm in the photo) along the design's right edge (= plane rows near 0); the
  other edges were covered. Cause on the driver side: CUPS hands over a card-size design (54 × 85.6 mm PDF, or a
  638×1011 image) at its own size, and the filter centred it in the 1036×664 plane, leaving ~12 white dots on
  every side, whereas the Windows driver forces the GDI page to 1036×664 (§1.5), so applications draw into the
  bleed. With the printer's own position offset, that border became visible on one side. The filter now scales
  pages to the full plane (filter `CardFit=Fill`, queue default `print-scaling=fill`) and adds a position correction
  in plane coordinates (`ImageShiftX/Y`, mm). [obs: Docker test of the CUPS pipeline; printer offset inferred
  from the photo]
- Linux cards were **more yellow** than Windows. The Windows settings on that PC: Portrait, Print on both sides, 3. YMCK both sides, K "Extraction / Only text / With YMC ink",
  **look-up table off**, colour adjustment and printer settings off, turn-over unit installed. The Matica LUT
  shifts the card's green (0,110,40) from C,M,Y = 255,145,215 to 224,149,252 (+37 Y, −31 C), which explains it.
  The driver now leaves the LUT off and uses K "with YMC" by default, matching that setup.
- CUPS and portrait duplex [obs, Docker]: with the landscape PageSize, cups-filters rotates a portrait page into a
  landscape raster in the same direction as the Windows driver (front identical), but the filter then applies
  the landscape back-side rule (180°), so the back is upside down relative to Windows. With
  `PageSize=CR80Portrait` both sides match the Windows portrait mapping. The filter cannot tell a rotated
  portrait page from a landscape design, so portrait jobs must use the portrait size (`xid8300-setup --id-card`).
- [unk] Whether the settings command will be accepted once the panel is idle; whether the XID8100 differs from
  the XID8300 in any later step (film/transfer commands, planes, options). Size of the printer's own image
  offset (needs test card 5 printed with the current driver).

---

## 11. Printing preferences (PDR23UIx64.DLL) → output [obs unless noted]

Dialogs dumped from the UI DLL resources; control → DEVMODE writes read from the dialog procedures (image base
0x6a900000), and the GD writer 0x7190a7f0. Defaults (GD 0x71901bb0 / UI 0x6a902f70): DM+0x5a40 = 0, all
printer-setting bytes 0xFF (not sent), 0x5a5f = 1, 0x5a61 = 1, 0x5a90 = 0xFF.

**0xC0 items.** `H F J K G U V _ L M N ] ^ O P` are written only when DM+0xec ≠ 0 and DM+0x5a40 = 1 ("Enable the
printer settings"); `(`, `)`/`*`, 0xF0, 0xF1 only with magnetic encoding on; 0xF2 with magnetic or IC encoding;
0xC8 whenever ≠ 0xFF. The port monitor forwards only F,G,H,I,J,K,L,M,N,O,P,U,V,W,[,(,),*,0xC8,],^,_ (0x5040c014);
F0/F1/F2 stay in the monitor.

| Windows control (dialog 136 "Printer settings" unless noted) | DM | id | value |
|---|---|---|---|
| YMC / Resin K / UV / Peel-off ink print level (slider −3 light … +3 dark) | 0x5a44–47 | H / F / J / K | 0..6 |
| Resin K print mode Standard / Fine | 0x5a49 | G | 0 / 1 |
| MG peel mode Standard / MG Stripe | 0x5a4c | U | 0 / 1 |
| Standby mode Front wait / Back wait | 0x5a4d | V | 0 / 1 |
| Back side cooling Enable / Disable | 0x5a53 | _ | 1 / 0 |
| Retransfer grid: temperature −2..+2 (row, shared by both sides) | 0x5a4e | L | temp + 2 |
| Retransfer grid: speed −3..+2, front / back | 0x5a4f / 0x5a50 | M / N | 2 − speed |
| UV speed front / back (slider 0..5, direction unknown) | 0x5a54 / 0x5a55 | ] / ^ | 0..5 |
| Bend remedy grid: temperature −5..0, speed −2..+2; "Disable" | 0x5a51 / 0x5a52 | O / P | temp + 5, speed + 2; Disable = 10, 4 |
| Encode: Write retry 0..3 | 0x5a5c | ( | 0..3 |
| Encode: Coercivity Lo-Co / Hi-Co | 0x5a5d | `*` (ISO encoder) or `)` (JIS) | 0 / 1 |
| Encode: Security | 0x5a5e | 0xF0 | not forwarded to the printer |
| Encode: Starts from magnetic encoding | 0x5a5f | 0xF1 | 4 = on (monitor skips IC in the first pass) |
| Encode: Prints after encoding | 0x5a60 | 0xF2 | 1 = encode before printing |
| Laminate (dialog 105) Laminate / Pass through | 0x5a90 | 0xC8 | 0 / 1 |

Unchecking a group writes 0xFF to its bytes; checking Retransfer without clicking a cell sends only `]`/`^`.
[inf] "Top right = more force" means higher temperature and slower speed (strings 4674/4676).

**Job header.** Setup "Turns over the card after loading" = DM+0xed bit1 → header +8 bit0 (LoadCard flag 0x10);
"Turns over the card before ejection" = DM+0xed bit0 → header +4 bit0, which the monitor masks off (0x50407b87:
`& 0xfe`) and never uses; Print "Print the back side first" = DM+0xed bit4 → header +8 bit 12. Encode page:
"Turns over the card after MG/IC encoding" = bit0 of header +6 / +7 (so 0x11 is *not* Hi-Co).

**K modes** (Print page): Page split = 0x40; Extraction + Only text = 0x10; Extraction + All = 0x20; "With YMC ink"
adds 1. "Only text" takes K from DrvTextOut calls in black (0x719145f0). Security erase = K-secure (0xfc/0x115c).

**Colour adjustment** (dialog 132; GD 0x71905280, only when DM+0x22a8 = 1, on RGB before the CMY step):
brightness, contrast, hue R/G/B −100..100 (0x22ac, 0x22b0, 0x22c0–c8), gamma R/G/B 1.0..10.0 in tenths
(0x22b4–bc). Pure white and pure black pixels are skipped. Per pixel: brightness adds bri·255/100; contrast table
`(i−127.5)·tan((con·0.45+45)°)+127.5`; gamma `(i/255)^(10/g)·255+0.5`; hue h on channel X raises X by i·h/100 and
lowers the other two by half that. Implemented in the Linux filter.

**Dither** (dialog 134): slider 10..30 (default 20) used only for K mode 0x40: gamma v/10, luminance
(30R+59G+11B)/100 (≥250 → 255), Jarvis-Judice-Ninke error diffusion, threshold 128.

**UV ink** (dialog 135): source byte 0x104/0x1164 (Page split 0x40; Extraction 0x10 + Text 1 + File 2);
"MAC address position" Upper right 1 / Lower left 2 → 0x87 header param high byte (swapped when rotated);
"Number of panels using the retransfer film" Auto/1/2 → param low byte 0/1/2; up to 8 BMP files placed by X/Y
(1/100 mm) per side.

**UI only:** Configuration page unit flags (only the ISO/JIS magnetic encoder type reaches the GD), Load/Save,
Version, the unlabelled checkboxes 3234/3236.
