# Release validation

Each release runs the C++/QML suites, compiler and sanitizer matrix,
100,000-event performance smoke, staged installation checks and clean Arch
package lifecycle. Widget validation covers IPC recovery, keyboard controls,
layout scales, source packaging and the Omarchy host API.

Manual acceptance covers account authentication, event editing, calendar sets,
visibility, invitation behavior and widget handoff on the owner's desktop.
Provider behavior varies; supported limitations are in [SUPPORT.md](../SUPPORT.md).
Release approval does not imply certification of every CalDAV deployment.

For bug reports, reproduce with a disposable calendar and redact private data.
