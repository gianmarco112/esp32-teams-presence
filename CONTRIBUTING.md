# Contributing

Thanks for your interest in improving Teams Presence for ESP32.

## Before opening an issue

- Search existing issues to avoid duplicates.
- Include the ESP32 board/module, firmware version, browser (if relevant), and a concise description of the problem.
- Remove Tenant IDs, Client IDs, access tokens, refresh tokens, Wi-Fi credentials, serial numbers, and other private information from logs and screenshots.

## Development setup

1. Fork the repository and clone your fork.
2. Install Visual Studio Code with PlatformIO IDE, or install PlatformIO Core.
3. Copy `include/config.h.example` to `include/config.h`.
4. Build the firmware with `pio run -e esp32dev`.
5. Test changes on the target hardware when possible.

Keep `include/config.h`, credentials, tokens, generated binaries, and local environment files out of commits.

## Pull requests

- Keep changes focused and explain the reason for the change.
- Describe hardware and firmware testing performed.
- Update documentation when behaviour, configuration, wiring, or setup steps change.
- Do not introduce client secrets or hard-coded organisation-specific identifiers.
- Confirm that the PlatformIO build succeeds before requesting review.

By submitting a contribution, you agree that it will be distributed under the repository's MIT License.
