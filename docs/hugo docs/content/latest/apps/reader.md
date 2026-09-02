---
title: "E-paper Reader"
description: "Read comics and books from an SD card on CrowPanel e-paper displays"
keywords: ["reader", "comic", "comics", "CBZ", "EPUB", "book", "e-paper", "SD card", "CrowPanel"]
weight: 20
---

The E-paper Reader is a built-in app for the Elecrow CrowPanel 4.2-inch 400×300 and 5.79-inch 792×272 e-paper displays. It reads content directly from the SD card and is designed around the display's slow refresh and physical buttons.

The Reader is included in both CrowPanel e-paper firmware builds. It does not download, copy, or store books in flash.

## SD card layout

Insert the SD card into a computer and create the following directory at the card's root:

```text
ghostesp/
└── comics/
    ├── MyComic/
    │   ├── 001.jpg
    │   └── 002.jpg
    ├── sample.cbz
    ├── sample.epub
    └── notes.txt
```

GhostESP creates `ghostesp/comics` automatically when the SD card is initialized. The device path for this directory is `/mnt/ghostesp/comics`.

The Reader scans the items directly inside `comics`. A folder is treated as one book, and its image pages must be directly inside that folder. Use zero-padded filenames such as `001.jpg`, `002.jpg`, and `003.jpg` so pages sort in reading order.

## Supported content

- **CBZ:** ZIP comic archives containing JPG/JPEG, BMP, or PBM image pages directly in the archive. Encrypted archives are not supported.
- **Image folders:** A folder containing directly stored JPG/JPEG, BMP, or PBM pages.
- **Single images:** JPG/JPEG, BMP, or PBM files placed directly in `comics`.
- **Text books:** `.txt` and `.md` files. Text is paginated automatically for the e-paper screen.
- **EPUB:** Standard, unencrypted EPUB 2/3 reflowable books. The Reader follows the package container, OPF manifest, and spine, then reflows XHTML chapters into readable e-paper pages. HTML entities, headings, paragraphs, lists, and chapter breaks are handled.

DRM-protected EPUBs and fixed-layout/image-only EPUBs are not supported by the lightweight on-device reader. Convert those books to CBZ (for comics) or a reflowable EPUB before copying them to the card.

For the most reliable first test, create a folder such as `MyComic` and place two ordinary JPG files inside it. CBZ archives should not add an extra top-level directory around the page files.

## Opening a book

1. Boot the CrowPanel with the SD card inserted.
2. Select **Reader** from the main menu (it is the first item on the e-paper build).
3. Select a book and press **OK**.
4. In a comic, press **OK** again to enter 2× detail mode.
5. Wait for the e-paper refresh to finish before pressing another page button.

## Controls

| Control | Library | Reading |
| --- | --- | --- |
| **PREV** | Previous book | Previous tile/section, then previous page |
| **NEXT** | Next book | Next tile/section, then next page |
| **OK** | Open selected book | Toggle overview/detail for comics; next page for text/images |
| **EXIT** | Leave Reader | Return to library |
| **HOME** | Return home | Return home |

The Reader uses the same theme, status bar, selection styling, and navigation behavior as other native GhostESP views. Page images are converted to black and white. Comic overview mode shows the complete page in vertical sections; detail mode enlarges it and visits tiles left-to-right, then top-to-bottom. NEXT advances to the next page after the last tile.

The 400×300 panel is a 1-bit display, so detail mode is the practical way to read small lettering. It cannot make text that is physically smaller than a display pixel readable while also showing the entire high-resolution page at once.

## Troubleshooting

### No books appear

Confirm that the physical SD card contains `ghostesp/comics`, not a folder named `/mnt/ghostesp/comics`. Insert the card before booting, then exit and reopen Reader after adding files.

Check that:

- The SD card is mounted and readable.
- The file extension is supported.
- A folder book has image pages directly inside it.
- A CBZ archive has image pages directly inside the archive.
- An EPUB is unencrypted and reflowable; fixed-layout EPUBs need to be converted to CBZ.

### A page cannot be opened

Try a baseline RGB JPG or a 400×300 JPG first. Unsupported, encrypted, damaged, or unusually encoded image files may show an error page. For EPUBs, verify that `META-INF/container.xml` points to an OPF package and that the OPF has a valid XHTML spine.

### Refreshes seem slow

This is expected on e-paper. The Reader waits for the panel's busy signal and throttles page input to avoid overlapping refreshes.
