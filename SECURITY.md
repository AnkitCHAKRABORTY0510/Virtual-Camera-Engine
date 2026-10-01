# Security Policy

## Supported versions

Security fixes are made for the latest release.

| Version | Supported |
|---|---|
| 1.1.x | ✅ |
| < 1.1 | ❌ |

## Reporting a vulnerability

Please **do not open a public issue** for security problems.

Use GitHub's private reporting instead: **Security → Report a vulnerability** on this repository.
Include the version, your Linux distribution, steps to reproduce and, if possible, a sample input file.

You will get an answer within a week. Once a fix is ready it is released and the reporter is credited
(unless they prefer otherwise).

## Things worth knowing

- The program decodes untrusted media with FFmpeg. Keep your distribution's FFmpeg packages up to date.
- Script input (`--source-type push`) listens on a Unix socket that is created with permissions `0600`,
  so only the same user can send frames.
- The setup script loads a kernel module and therefore needs `sudo`; the camera program itself does not.
