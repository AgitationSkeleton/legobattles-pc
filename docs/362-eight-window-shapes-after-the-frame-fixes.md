# 362 — Eight window shapes, after the frame fixes

Written for: whoever checks window sizes next.

Status: **measured; nothing changed.** The build carrying docs/358-361
(`7e57ab74`, deployed), the reference replay, a quiet machine (1-18% CPU load
before each run).

## 1. Method

The replay at eight window shapes, one run each. The test display is
1920x1080, so the three shapes wider than it are
reproduced at pixel scale 1 with the surface they would get at scale 2
(frame cost follows the surface, not the window). The frame figures are the
harness's 6 s window right after the replay ends, in battle. Layer dumps
every 1,100 composites keep them out of that window. `lb_thinholes.py`
screens every layer map for fog holes.

## 2. Results

| window | shape | surface | fps | emulation | composite | idle | frames > 16.7 ms work | > 20 ms | cadence sd | underruns |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 640x480 | 4:3 | 320x240 | 59.86 | 8.1 ms | 1.30 ms | 6.7 ms | 1 | 0 | 0.20 ms | 0 |
| 1024x768 | 4:3 | 512x384 | 59.82 | 10.2 | 0.68 | 5.2 | 2 | 1 | 0.23 | 0 |
| 1280x720 | 16:9 | 640x360 | 59.83 | 8.8 | 0.72 | 6.5 | 0 | 0 | 0.15 | 0 |
| 1920x1080 | 16:9 | 960x540 | 59.82 | 9.9 | 1.43 | 4.5 | 2 | 1 | 0.29 | 0 |
| 1080x1350 | 4:5 portrait | 540x674 | 59.82 | 10.0 | 1.13 | 4.9 | 0 | 0 | 0.17 | 0 |
| 2560x1080 | 21:9 | 1280x540 | 59.86 | 11.0 | 1.87 | 2.9 | 5 | 0 | 0.18 | 0 |
| 3440x1440 | 21:9 | 1536x642 | 59.85 | 10.7 | 2.58 | 2.3 | 53 | 2 | 0.60 | 0 |
| 3840x1080 | 32:9 | 1536x432 | 59.82 | 10.3 | 1.83 | 3.7 | 7 | 0 | 0.30 | 0 |

Every shape holds 60, with 2.3-6.7 ms of idle a frame and no audio
underrun. For comparison, docs/352's sweep (09-23) had 3440x1440 at 58.9
fps with 44 underruns, and this morning's baseline had it at a 2.13 ms
cadence sd (docs/358).

Two readings:

- **The widest shapes are map-limited.** The port raises the pixel scale
  so the window never shows more than the map (`lb_world_extent`), so 21:9
  at 3440 and 32:9 at 3840 both get the replay map's full width, 1,536
  columns. The 32:9 window is filled at 1.25x, not pillarboxed. The
  1,728-column ceiling (`kMaxAdaptiveWidth`) does not bind on this map.
- **The 320x240 composite runs on one thread**
  (`lb_parallel_rows` skips the pool below 256 rows). That is why it costs
  1.30 ms against 0.68 ms at 512x384. The frame still has 6.7 ms idle, so
  there is no reason to change it.

## 3. Rendering

- **Fog holes:** 56 layer maps across the eight shapes (7 per run, inside
  and after the replay), `lb_thinholes.py` 0 on every one.
- **Looked at:** the 32:9 composite at 1536x432 and the portrait one at
  540x674, mid-battle. The explored ground, its forest edge and the base are
  whole. The fog is organic around the explored area and solid over the
  unexplored east (32:9). The portrait view extends the field downward with
  the map's own edge terrain. No seam, hole, stale tile or band artefact.
- **The native-picture skip (docs/361)** skipped every line of about 7,500
  battle frames per run at every shape, and no run ever had to show a
  skipped native picture.
