# Release screenshot capture

`capture-release-screenshots.sh` renders OmaCalendar's production `Main.qml`,
views, components, and theme through Qt 6's software renderer. It supplies a
screenshot-only in-memory application fixture containing synthetic calendars,
events, and tasks. The fixture is not included in normal builds and does not start the
daemon, read the OmaCalendar database, access Secret Service, or contact a
provider.

Run from any directory:

```sh
./tools/screenshots/capture-release-screenshots.sh
```

The script uses temporary XDG directories and writes the five stripped PNGs to
`docs/screenshots/`. It requires Qt 6 `qmltestrunner`, ImageMagick, Tesseract,
ripgrep, and a font such as DejaVu Sans (`ttf-dejavu` on Arch). Before succeeding it verifies dimensions, recognizable OCR text, embedded
strings, and the absence of image profiles associated with metadata.

The images are also copied to `site/screenshots/` for the public gallery.

Keep the fixture free of real names, addresses, account identifiers, email
addresses, paths, tokens, and provider secrets. Visually inspect every image
after regeneration; OCR is a backstop, not a substitute for review.
