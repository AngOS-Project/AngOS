# Contributing to AngOS

Thank you for your interest in contributing to **AngOS**!

You **do not need to be an OS expert** to contribute. If you're interested in operating systems, low-level programming, or simply want to learn, you're welcome to contribute!

## Ways to Contribute

There are many ways to help AngOS. For example:
* Kernel development
* Bootloader development
* Memory management
* Interrupts and hardware support
* Device drivers
* Filesystems
* Networking
* Shell development
* Build system improvements
* Documentation
* Testing
* Bug reports
* Bug fixes
* Code review
* Ideas and feature proposals
And many more!
*(Practically anything that has to do with AngOS can be counted as a contribution!)*

## Getting Started

The easiest way to contribute code is through a **fork and Pull Request**.

### 1. Fork AngOS

Create a fork of the AngOS repository on GitHub.

Your fork will be your own copy of the project where you can make changes without needing direct write access to the AngOS repository.

### 2. Clone your fork

Clone your fork to your computer or development environment.

### 3. Create a branch

Create a separate branch for your changes.

For example:

```text
feature/keyboard-driver
fix/filesystem-bug
docs/build-instructions
```

Avoid making changes directly to your fork's `main` branch when possible.

### 4. Make your changes

Make your changes and keep them focused.

If you're adding a new feature, try to keep unrelated changes out of the same Pull Request.

### 5. Test your changes

Before submitting a Pull Request, make sure your changes build successfully and test them as much as reasonably possible.

For changes to the kernel or other low-level components, testing with QEMU is encouraged.

If something cannot be tested yet, explain that in your Pull Request.

### 6. Open a Pull Request

Once your changes are ready, open a Pull Request from your fork to the AngOS repository.

You don't need to have write access to AngOS to submit a Pull Request.

## Pull Requests

Pull Requests are how changes are proposed to the main AngOS repository.

Every Pull Request will be reviewed before being merged.

The AngOS repository maintainers decide whether a Pull Request is accepted, requires changes, or is closed.

A Pull Request may be:

* ✅ **Merged** if the changes are suitable for AngOS.
* 🔄 **Updated** if changes are requested.
* ❌ **Closed** if the changes aren't suitable for the project.

Opening a Pull Request does not guarantee that it will be merged.

This allows anyone to contribute while keeping the official AngOS repository stable and allowing the project maintainers to decide what becomes part of AngOS.

## Issues

If you find a bug, have an idea, or want to discuss something about AngOS, you can open an Issue.

When reporting a bug, please provide as much useful information as possible, including:
* What happened
* What you expected to happen
* Steps to reproduce the problem
* Whether you were using QEMU or real hardware
* Relevant build or error output
* The AngOS version or commit you were using

For feature requests, explain:
* What you want to add
* Why it would be useful
* How you think it could work, if you have an idea

## Third-Party Software

AngOS may incorporate software originally developed by other individuals and projects.

These components may be modified, adapted, or integrated into AngOS to meet the project's technical requirements.

AngOS aims to use only software that is:

* In the public domain
* Released under the **CC0 1.0 Universal (CC0)** license
* Released under **The Unlicense**
* Released under the **Zero Clause BSD (0BSD)** license
* Released under the **MIT No Attribution (MIT-0)** license

This policy is intended to keep **all** third-party software used by AngOS under the AngOS License so all rights are retained.

### Before submitting third-party code

**Do not copy code from another project into AngOS without checking its license first.**

Before contributing code derived from another project, verify that:

1. The project's license is compatible with AngOS's third-party software policy.
2. You have the right to submit the code.
3. By contributing the code to AngOS, you agree that it may be distributed under the AngOS license.

When in doubt, **ask before submitting the code**.

Do not assume that code being publicly available on GitHub means that it can be freely incorporated into AngOS.

## Original Contributions

By submitting original work to AngOS, contributors agree that their contributions may be incorporated into AngOS and distributed under the AngOS project's license.

Contributors grant AngOS permission to use, modify, combine, monetize, and redistribute their contributions as part of the project.

Contributors must not submit work that they do not have the right to submit.

## Code and Changes

When contributing code:

* Keep changes understandable.
* Avoid unnecessary complexity.
* Keep changes relevant to the Pull Request.
* Document complicated code where appropriate.
* Test your changes when possible.
* Don't introduce third-party code without checking its license.
* Don't intentionally break existing functionality without a good reason.

Because AngOS is in early development, existing code and APIs may change significantly.

## Not Sure Where to Start?

That's completely fine!

Look through the GitHub Issues for things that interest you.

Issues labelled **`good first issue`** are intended to be easier starting points for new contributors.

Issues labelled **`help wanted`** indicate areas where additional contributors would be particularly useful.

You can also open an Issue and say what you would like to work on.

You don't have to wait until you're an expert.

**AngOS is being built by people who want to learn, experiment, and create an operating system together.**

## Pull Request Checklist

Before submitting a Pull Request, please check:

* [ ] My changes are related to the purpose of the Pull Request.
* [ ] AngOS builds successfully, if applicable.
* [ ] I tested my changes as much as reasonably possible.
* [ ] I have not knowingly included incompatible third-party code.
* [ ] I have the right to submit the code.
* [ ] I have explained anything that could be useful for reviewers.
* [ ] I have updated relevant documentation, if necessary.

## Thank You!

Whether you contribute code, documentation, testing, bug reports, ideas, or feedback, **thank you for helping AngOS grow.**

Every contribution is appreciated!
