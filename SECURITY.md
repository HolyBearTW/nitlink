# Security

NitLink runs as a normal user process. It reaches capture hardware through Media Foundation and the vendor control paths documented in this repository, routes audio through WASAPI, and embeds a WebView2 panel that loads only the bundled settings page. It makes no network requests of its own.

## Reporting a vulnerability

Report privately through the **Report a vulnerability** button on this repository's Security tab rather than in a public issue. Include the NitLink version, the capture card, and steps to reproduce.

Reports get a reply on the advisory thread. A fix ships in the next release, with credit to the reporter unless they prefer otherwise.

## Supported versions

Only the latest release receives fixes.
