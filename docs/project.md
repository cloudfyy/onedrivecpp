# License, legal, and branding

English | [简体中文](project.zh-CN.md) |
[Documentation index](README.md)

## License

GPL-3.0-or-later. The reference project also uses GPLv3, which makes this
license suitable for future reuse or adaptation of its design in compliance
with the license terms.

The full GPLv3 text and the project's "or any later version" notice are in
[LICENSE](../LICENSE). CMake installations and DEB packages include `LICENSE`
and the machine-readable `copyright` file in the documentation directory,
normally `<prefix>/share/doc/onedrive-cpp` (`/usr/share/doc/onedrive-cpp` for
standard DEB packages). When FTXUI/Proxy sources are bundled, their original
licenses are also installed in the `licenses` subdirectory.

## Project and package metadata

The project source is [cloudfyy/onedrivecpp](https://github.com/cloudfyy/onedrivecpp).
`debian/control` defines the package maintainer, homepage and description;
CPack reads the same fields rather than maintaining separate copies.
The reference project mentioned above is not the source of this C++ implementation.

## Legal and Branding

- [Terms of Service](../TERMS.md)
- [Privacy Statement](../PRIVACY.md)
- Project logo: [SVG](../assets/onedrive-cpp-logo.svg) |
  [512x512 PNG](../assets/onedrive-cpp-logo.png)

The legal documents are project-maintainer drafts, not legal advice. Review
them for the applicable organization and jurisdiction before a public or
commercial release. onedrive-cpp is an independent project and is not
affiliated with or endorsed by Microsoft.
