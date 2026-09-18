# Contributing to Equinox OS

Thank you for your interest in improving Equinox OS. This project is a small, experimental 32-bit x86 operating system with a monolithic kernel, FAT32 disk support, network stack, userland runtime, and QEMU-based development workflow.

We welcome contributions that improve correctness, stability, portability, documentation, build tooling, and developer experience.

Please read this guide before opening an issue or pull request.

## Project scope

Equinox OS is a research and hobbyist operating system project. It is intended for learning, experimentation, emulation, and development in QEMU.

It is not a production-ready operating system and should not be treated as one. Changes affecting boot code, paging, memory layout, driver behavior, or file-system persistence should be tested carefully.

## Code of conduct

We expect all contributors to be respectful, constructive, and technically focused. In particular:

- Be respectful in issues, discussions, and pull requests.
- Keep feedback specific and actionable.
- Avoid dismissing experimental work without evidence.
- Prefer clear technical explanations over vague claims.
- Do not propose changes that are unsafe without documenting the risks.

## Ways to contribute

You can contribute in many ways:

- Fix bugs or crashes in kernel code, drivers, or userland.
- Improve filesystem correctness and persistence.
- Add coverage to host-side and QEMU-based tests.
- Improve documentation, build steps, and developer onboarding.
- Add support for new devices, commands, or debugging tools.
- Improve memory safety, error handling, and validation paths.
- Refactor large subsystems into cleaner, more maintainable components.

## Before you start

Before making a change, please:

1. Check whether an issue already exists for the behavior you want to change.
2. Keep your patch focused on one concern.
3. Prefer small, reviewable changes over broad rewrites.
4. Understand the project constraints:
   - This is a freestanding 32-bit kernel.
   - There is no standard hosted libc/runtime.
   - Interrupt context and kernel code must be careful with stack usage and register state.
   - QEMU is the primary validation environment.

## Repository layout

The repository is organized around the source tree under `src/`:

```text
src/
├── boot/
├── kernel/
├── mrp_user/
├── games/
├── Libgame/
├── doomgeneric/
├── test/
├── scripts/
├── third_party/
├── makefile
├── linker.ld
├── README.md
└── ...
```

The top-level project README is the primary user-facing overview. Project source and build logic live primarily under `src/`.

## Development environment

### Required tools

Typical Linux development setup:

- `gcc` / `g++` with 32-bit support or an `i686-elf` cross toolchain
- `nasm`
- `grub-mkrescue` or equivalent GRUB toolchain components
- `xorriso`
- `mtools`
- Python 3
- `qemu-system-i386`
- GNU make

If you are using a host compiler mode, make sure the correct 32-bit libraries are available.

### Build from source

From the project root:

```sh
cd src
make
```

Useful targets:

```sh
make pack
make mtcc
make doom
make diskimg
make run
make run-disk
make test
make test-fat32
make test-doom-disk
make clean
```

## Coding standards

### General principles

- Prefer simple, explicit code over clever abstractions.
- Avoid undefined behavior, especially around pointer arithmetic and memory ownership.
- Keep kernel and interrupt-sensitive code conservative and predictable.
- Use bounded operations for names, buffers, and path handling.
- Do not rely on hosted libc, runtime, or process semantics.
- Preserve the project’s existing conventions unless a clear improvement is required.

### C and C++ style

- Prefer readable, direct code.
- Keep naming patterns coherent with the surrounding subsystem.
- Use comments sparingly but clearly when behavior is subtle or non-obvious.
- Explain hardware assumptions, boot-order dependencies, and memory layout constraints where relevant.
- Avoid adding silent workarounds in security-sensitive code paths.

### Safety-focused expectations

Because this project operates in ring 0 and manipulates low-level system state, contributors should be especially careful with:

- stack overflow risk
- kernel heap lifetime and ownership
- pointer validity and page alignment
- interrupt reentrancy
- DMA and I/O ordering assumptions
- disk writes and rollback semantics
- file-system metadata consistency
- unvalidated user input in shell or syscall paths

If a fix touches boot code, paging, filesystem writes, or memory layout, it should be validated with a rebuild and a boot test.

## Testing expectations

All non-trivial changes should be tested before submission.

### Minimum validation

At minimum, run:

```sh
cd src
make clean
make
make test
```

### Relevant testing by area

#### Kernel or boot changes

- Rebuild the kernel and ISO
- Boot in QEMU
- Verify the full boot log and shell prompt appear correctly
- Check for crashes, freezes, or unexpected reboots

#### Filesystem or disk changes

- Run FAT32 tests when applicable
- Validate read/write persistence behavior
- Test with both normal boot and disk-backed boot where relevant

#### Userland or MRP changes

- Rebuild packed user programs
- Validate that executables load and run correctly in ring 3
- Verify expected command output and failure conditions

#### Networking changes

- Validate the relevant HTTP, DNS, ICMP, TCP, or TLS paths
- Check that changes do not silently break the guest networking model

## Pull request process

### Before opening a PR

Please ensure:

- The change is focused and easy to review.
- Relevant tests have been run.
- Build output and generated artifacts are not accidentally committed.
- The branch is clean and rebased onto the current project state if needed.
- The PR description clearly explains the purpose and validation performed.

### PR checklist

A good PR should include:

- a concise title
- a clear summary of the change
- a justification for the fix or feature
- affected files and subsystems
- validation commands run
- any known limitations or follow-up work

Example structure:

```markdown
## Summary
Fix FAT32 directory growth when a new cluster is added after the first metadata update.

## Changes
- validate cluster-chain growth in the write path
- ensure dirent metadata is refreshed after allocation
- add a regression check for the failing case

## Validation
- make clean
- make
- make test-fat32
- QEMU boot smoke test
```

### Review expectations

We expect PRs to be understandable and technically defensible. Review comments may request:

- additional validation
- better separation of responsibilities
- clearer comments or documentation
- simpler or safer implementation choices
- additional regression tests

## Issue reporting

When opening an issue, please include:

- the exact command or reproduction steps
- the expected behavior
- the actual behavior
- any build or QEMU command used
- host environment details
- the platform/toolchain version
- relevant log output or screenshots

For kernel crashes, hangs, or boot failures, a complete boot log is especially valuable.

## Security and safety notes

This project is a low-level system project. Please keep the following in mind:

- Do not claim security guarantees that are not tested and documented.
- Be explicit about any partial verification or warned fallback modes.
- Do not add features that silently bypass safety checks.
- Treat certificate validation and trust decisions carefully.
- Avoid broad or unsupported claims about real hardware compatibility.

If a change affects boot correctness, paging, memory safety, or I/O path reliability, it should be documented clearly in the PR.

## Good contribution examples

We especially value:

- reproducible bug fixes with targeted tests
- small, well-documented kernel improvements
- filesystem correctness improvements
- build-system fixes and portability improvements
- better shell behavior and diagnostics
- refactors that reduce complexity without changing the project’s intended behavior

## Questions

If you are unsure whether a change fits the project, open a discussion or issue first. This helps avoid wasted work and keeps the project direction consistent.

We appreciate thoughtful contributions and technical rigor.

Thank you for helping improve Equinox OS.
