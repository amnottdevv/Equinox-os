# Security Policy

## Supported versions

The project currently supports the latest development branch and the most recent published release.

Because Equinox OS is an experimental 32-bit hobby operating system running in emulation and bare-metal development environments, support is focused on the current source state rather than long-lived stable branches.

| Version | Supported |
| --- | --- |
| main / current development branch | Yes |
| latest published release | Yes |
| older releases | No |

If you are reporting a security issue, please assume that the latest available source is the reference target unless the issue is clearly tied to a specific release tag.

## Reporting a vulnerability

Please do not open a public GitHub issue for security vulnerabilities.

Instead, report the issue privately through one of the following channels:

- GitHub Security Advisories (preferred): use the repository's private reporting flow if available
- Email the repository maintainer directly if a private contact is explicitly published in the repository

Please include the following information in your report:

- a clear description of the issue
- affected component or subsystem
- reproduction steps or proof of concept
- build and environment details
- expected vs. actual behavior
- any relevant log output, crash dumps, or QEMU output
- whether the issue affects boot, memory, filesystem, userland, networking, or the kernel shell

We ask that you provide a reasonable amount of detail so the maintainer can assess and reproduce the issue quickly.

## Response expectations

We aim to acknowledge valid security reports as quickly as possible. Response times may vary depending on project activity and available maintainer time, but we will make a good-faith effort to:

- acknowledge the report
- assess severity and impact
- investigate the issue
- prepare a fix or mitigation
- coordinate a public disclosure timeline when appropriate

## Disclosure policy

We follow a responsible disclosure approach:

- private investigation first whenever feasible
- fix and validation before public disclosure
- public advisory or release note only after a fix exists or is being prepared

This project is intentionally experimental and low-level. Some issues may be tied to unsafe assumptions, incomplete validation, or limitations in the QEMU-based environment rather than a conventional production security boundary.

## Security-sensitive areas

The most security-sensitive parts of the project include:

- boot code and early memory initialization
- paging and privilege transitions
- syscall validation and ring transitions
- filesystem metadata and persistence logic
- FAT32 write-through behavior
- network packet parsing and TLS trust decisions
- userland program loading and memory layout
- command parsing and shell input validation

If a report touches one of these areas, the maintainer may request additional validation and a careful reproduction before the fix is merged.

## Safe handling guidance

Contributors should assume that the following areas are high-risk and need careful review:

- direct memory writes
- raw hardware I/O
- pointer arithmetic and array bounds
- boot-time relocation or module copying
- untrusted input parsing
- persistent filesystem updates
- network parsing and redirect handling
- certificate validation and trust fallback behavior

When in doubt, prefer correctness and explicit validation over optimization or shortcuts.

## Security note for experimental work

Equinox OS is a hobby and learning project. It is not designed to be hardened against malicious untrusted software in the same way as a production operating system. While we do appreciate vulnerability reports, the project's threat model is intentionally narrow and centered on correctness, reliability, and developer workflow under QEMU and similar emulation setups.

## Public disclosures

Public disclosure will typically happen only after a patch is ready or a risk has been mitigated. If a vulnerability is fixed in a private branch or not yet released, the public discussion will be limited to the final remediation and any relevant impact information.

If you are unsure whether an issue qualifies as a security issue, please contact the maintainer privately before making it public.
