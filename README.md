# sortof-suite

Small programs that make an ordered list of files for an archiver. Similar files next to each other compress better, so the order of the list is a lever on the compressed size, and the programs here pull it in different ways. Each one does a single job, reads sources, and writes one path per line:

```
sortof-natural   DIR... > list.txt      # folder by folder, f2 before f10
sortof-extension DIR... > list.txt      # by extension, then name, then folder
sortof-minhash   DIR... > list.txt      # similarity order (MinHash)
sortof-cdcsort   LIST   > list.txt      # keeps the order; chains identical and near-identical files together
sortof-palscore  DIR... -o list.txt     # scene order for JPEG frame lists
```

The list goes straight to an archiver: `tar -T list.txt -cf - | zstd -9`, `zfrag pack -T list.txt`, or DwarFS with `--input-list list.txt --order=none`. Nothing here compresses or changes a file.

## Build

Needs gcc and make; `sortof-palscore` also needs libjpeg (`apt install libjpeg-dev`).

```
make
make install PREFIX=/usr/local
```

`make install` also installs the man pages (for example `man sortof-cdcsort`).

To add an orderer, write `sortof-NAME.c` that includes `sortof-common.h` and add NAME to `PROGS` in the Makefile. To drop one, take it out of `PROGS` and delete the file.

## What they share

* **Sources.** Every program takes any mix of directories (scanned recursively), regular files, and `-` (paths from stdin, once). A path that comes up more than once is listed once. `find DIR -type f | sortof-cdcsort -` works.
* **Output.** The list goes to stdout, or to a file with `-f PATH`. `-q` is quiet, `-h` is help.
* **Pinned files.** The files a format needs are handled for you: a `cdcstore.idx` in the input comes out first in every tool, and `sortof-palscore` adds the `specs.txt` of every set it lists (the mount needs it). `-p PATH` (repeatable) is for your own files: it puts PATH first in the output, exactly as given, and never sorts, hashes or reads it.
* **Same input, same list.** The natural orders do not depend on the locale or on the order the disk lists files in.

## The tools

### sortof-natural and sortof-extension

No hashing and no reading of file contents. `sortof-natural` lists path by path, one folder level at a time, with digit runs compared as numbers (`f2` before `f10`), and every folder's files stay together. `sortof-extension` sorts by extension (case ignored, none first), then file name, then folder, so a runtime file that many games carry (`DOS4GW.EXE`) ends up in one place.

### sortof-minhash

Computes a MinHash digest for every file (128 functions over 8-byte shingles), orders greedily so files that share byte sequences end up adjacent, then refines with 2-opt sweeps. It starts from the first file, so give it a sorted list (`sortof-natural dir | sortof-minhash -`). Options: `-b N` files per batch (default: one batch, most accurate), `-o N` refinement sweeps (default 50, 0 off), `-m` Matroska-aware (hash only the Cluster payload of real `.mkv` files), `-t N` threads, `-v` print a memory estimate first.

On a 12.6 GB DOS collection MinHash beat the nilsimsa order it replaced by 1.17% under `zstd -9` and 1.26% under xz. Shuffling the input order alone moves a result by up to about 0.25%.

### sortof-cdcsort

Keeps the list in the order it is given, except that files whose content matches an earlier file are moved right behind it:

* **Identical files** (same length and 64-bit FNV hash) come directly behind the first copy. Only files that share a size with another are read, and a wrong match could only change the order, never the data.
* **Near matches** (`-s PCT`, default 50): a file whose MinHash sketch is at least PCT percent like an earlier file's moves behind the most similar such file. Candidates come from banding the sketch, with no all-pairs comparison, so it scales to millions of files. Every file is read once and a sketch takes 512 bytes of memory. `-s 0` chains exact duplicates only.

This is the one for `cdcstore` chunk lists: a compressor with a small window then finds the repeat next to the original. Other options: `-m N` candidates tried per band (default 16), `-e N` no sketch for files smaller than N bytes (default 512), `-t N` threads.

```
cdcstore-list -k REPO | sortof-cdcsort - > list.txt
```

### sortof-palscore

For lists of JPEG frames (from `mjpegstore`). Decodes every picture at 1/8 scale, reduces it to a small colour-histogram and layout signature, finds scenes, and writes the list in scene order: scenes stay whole and in natural order inside, and after each one the most similar unvisited scene follows if it is close enough (`-j PCT`, default 5). Byte-identical frames are chained behind their first copy (`-D`, on by default; `-N` turns it off). Signatures are kept in a file (`-w`) and reused while the list is the same, so tuning takes seconds.

Give it folders and it makes the natural list itself: `sortof-palscore SET/ep1 -o list.txt`. It adds each set's `specs.txt` to the end of the list by itself. `-v FILE` writes one line per scene. Run `sortof-palscore -h` for the scene settings (`-c`, `-k`, `-b`, `-g`).

## Which one

| data | use |
|---|---|
| anything, no idea what is in it | `sortof-natural` |
| a mixed collection (games, documents) | `sortof-extension` or `sortof-minhash` |
| a `cdcstore` chunk list, disc images, anything with repeated or patched files | `sortof-cdcsort` |
| JPEG video frames | `sortof-palscore` (much faster and tuned for video; `sortof-cdcsort` is within 0.1%, `sortof-natural` is fine when there are no repeats) |

## What the order is worth

`tar | zstd -9`, chunk lists of two `cdcstore` repositories, in the order the data was added and then through `sortof-cdcsort` (default `-s 50`):

| | files | chunk order | `sortof-cdcsort` |
|---|---|---|---|
| DOS collection | 622,981 | 6,662,700,321 | 6,103,466,864 (8.4% smaller) |
| Disc images | 73,102 | 921,588,375 | 718,117,377 (22.1% smaller) |

On video frames the order hardly matters to zstd. Three episodes in natural order gave 4,001,413,059 bytes and `sortof-palscore` with duplicate chaining 3,995,899,573, 0.14% less. Exact duplicates are where it counts: three copies of one episode gave 4,398,631,674 bytes in natural order, 3,819,174,102 with palscore alone, and 1,492,883,124 with duplicate chaining, because the chain puts a frame's twin one file away, inside any compressor window.

`sortof-cdcsort` and `sortof-palscore` on the same video sets (`tar | zstd -9`):

| | source | `sortof-palscore` | `sortof-cdcsort` |
|---|---|---|---|
| three episodes | 8,578,670,096 | 3,995,872,070 | 3,997,280,862 (0.04% larger) |
| three copies of one episode | 8,593,603,438 | 1,492,917,109 | 1,494,183,068 (0.08% larger) |

Both do the job, so picking the wrong sorter for video costs almost nothing. `sortof-palscore` is tuned for video and much faster, so use it there. Making the list from scratch (no signature file), wall-clock time and CPU time (user + system):

| | `sortof-palscore` | `sortof-cdcsort` |
|---|---|---|
| three episodes | 28 s wall, 91 s CPU | 5 min 28 s wall, 42 min 36 s CPU |
| three copies of one episode | 48 s wall, 98 s CPU | 1 min 52 s wall, 14 min 20 s CPU |

That is 12 times faster on wall time (28 times less CPU) for the three episodes, and 2.3 times faster (8.7 times less CPU) for the copies. `sortof-cdcsort` reads and sketches every byte of every file; `sortof-palscore` decodes a thumbnail of each picture. Both use all the cores of the machine.

## runall.sh

A test bench for the video case, not needed to use the tools: it encodes y4m episodes with `mjpegstore`, makes the natural and palscore lists for each restart setting, and prints the compressed size of each list. `bash runall.sh` without arguments prints its options.

## Questions and answers

**Why separate tools?**
It is easier to update and add sorters than to maintain a monolithic core. And who likes to remember which switch goes where? For the most part, each tool does what it needs to with its default settings.

**Why not embed the sorters in each tool?**
I wanted my other tools to work on their own and this suite to be a complement. Most of these tools can help other compressors too (DwarFS, zstd, xz, 7z and so on), so they are far more useful on their own than bundled.

## Thank you

* **Andrei Broder**, "On the resemblance and containment of documents", SEQUENCES '97, and **Broder, Charikar, Frieze and Mitzenmacher**, "Min-wise independent permutations", STOC 1998: MinHash, which `sortof-minhash` and `sortof-cdcsort` use to tell how alike two files are.
* **Piotr Indyk and Rajeev Motwani**, "Approximate nearest neighbors: towards removing the curse of dimensionality", STOC 1998, and **Jure Leskovec, Anand Rajaraman and Jeff Ullman**, *Mining of Massive Datasets*, chapter 3: locality-sensitive hashing and the banding that lets `sortof-cdcsort` find look-alikes without comparing every pair.
* **Glenn Fowler, Landon Curt Noll and Kiem-Phong Vo**: the FNV hash used to spot identical files (isthe.com/chongo/tech/comp/fnv).
* **[Debian](https://www.debian.org)**: for a solid distribution that does not move, which makes supporting a version for years instead of days possible.

## License

MIT. Copyright (c) 2026 Daniel Lee Witzel. See `LICENSE`.

### Third-party libraries

`sortof-palscore` links to libjpeg (libjpeg-turbo: IJG, BSD-3-Clause and zlib licenses). The other programs use no library. Nothing of libjpeg is included: it uses the copy installed on the system.

If you package or distribute binaries: with dynamic linking to the system libraries (as a Debian package does) nothing more is needed beyond shipping the `LICENSE` file. If you link a library statically or bundle it, you must also include that library's own license text, and for libfuse (LGPL) let users replace it with their own build.
