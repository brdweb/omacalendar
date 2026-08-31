# Release screenshot capture

`capture-release-screenshots.sh` renders OmaCalendar's production `Main.qml`,
views, components, and theme through Qt 6's software renderer. It supplies a
screenshot-only in-memory application fixture containing synthetic calendars
and events. The fixture is not included in normal builds and does not start the
daemon, read the OmaCalendar database, access Secret Service, or contact a
provider.

Run from any directory:

```sh
./tools/screenshots/capture-release-screenshots.sh
```

The script uses temporary XDG directories and writes the four stripped PNGs to
`docs/screenshots/`. It requires Qt 6 `qmltestrunner`, ImageMagick, Tesseract,
and ripgrep. Before succeeding it verifies dimensions, OCR output, embedded
strings, and the absence of image profiles associated with metadata.

Keep the fixture free of real names, addresses, account identifiers, email
addresses, paths, tokens, and provider secrets. Visually inspect every image
after regeneration; OCR is a backstop, not a substitute for review.
