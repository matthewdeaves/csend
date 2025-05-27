# Git Hooks for CSend

This directory contains Git hooks to maintain code quality.

## Available Hooks

### pre-commit
- Runs dead code detection on staged C files
- Checks only the platform(s) being modified
- Shows warnings for unused code
- Allows user to abort commit if issues are found

## Installation

To enable these hooks, run from the project root:

```bash
git config core.hooksPath .githooks
```

To disable hooks temporarily:

```bash
git config --unset core.hooksPath
```

To bypass a specific hook:

```bash
git commit --no-verify
```

## Requirements

- The deadcode_check.sh script must be present in the project root
- GCC must be installed for code analysis

## Customization

The pre-commit hook runs in "warnings" mode for speed. To make it more strict:
- Change `warnings` to `full` in the hook script
- Add additional checks for complexity or code duplication