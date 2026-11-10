# Security Policy

## Supported versions

Security fixes are intended for the latest version on the default branch. Older releases may not receive security updates.

## Reporting a vulnerability

Please do not disclose security vulnerabilities in public issues.

Use GitHub's **Private vulnerability reporting** feature for this repository (the Security tab → Report a vulnerability), if available. Include:

- a clear description and potential impact;
- affected firmware version, files, or components;
- reproducible steps or a proof of concept;
- any suggested mitigation.

Please allow reasonable time for investigation and a fix before public disclosure. Do not include real access tokens, refresh tokens, passwords, Wi-Fi credentials, or personal data in a report.

## Security notes

This project stores Microsoft OAuth refresh-token material in ESP32 NVS. Standard NVS storage is not automatically encrypted. The local device web interface currently uses HTTP and has no application-level password. Do not expose it directly to the Internet; use it only on a trusted network.

The firmware is a community project and is not an official Microsoft product. Review the threat model and harden the device before using it in sensitive environments.
