---
name: Bug report
about: Report a bug to help us improve Elio
title: '[Bug] '
labels: 'bug'
assignees: ''
---

## Bug Description

A clear and concise description of what the bug is.

## Environment

Please complete the following information:

- **Elio Version**: [e.g., 0.5.0]
- **Compiler**: [e.g., GCC 14.1, Clang 18]
- **OS**: [e.g., Ubuntu 24.04, Fedora 40]
- **Kernel Version**: [e.g., 6.8.0]
- **Build Type**: [e.g., Debug, Release, RelWithDebInfo]
- **CMake Version**: [e.g., 3.28]

## Steps to Reproduce

Steps to reproduce the behavior:

1. [First step]
2. [Second step]
3. [Third step]
4. [See error]

### Minimal Reproducible Example

If possible, provide a minimal code example that demonstrates the bug:

```cpp
#include <elio/elio.hpp>

using namespace elio;

// Your minimal example here
coro::task<void> reproduce_bug() {
    // ...
    co_return;
}

int main() {
    runtime::scheduler sched(2);
    sched.start();
    sched.go(reproduce_bug);
    // ...
    return 0;
}
```

## Expected Behavior

A clear and concise description of what you expected to happen.

## Actual Behavior

A clear and concise description of what actually happened.

### Error Messages

If applicable, include any error messages, stack traces, or logs:

```
Paste error output here
```

### ASAN/TSAN Output

If you're reporting a memory or threading issue, please include sanitizer output:

```
Paste ASAN/TSAN output here
```

## Additional Context

Add any other context about the problem here:

- Does the bug occur consistently or intermittently?
- Does it happen in Debug, Release, or both build types?
- Does it happen with ASAN/TSAN enabled?
- Any workarounds you've discovered?

## Related Issues

Are there any related issues or PRs? Link them here:

- #

## Possible Solution

If you have ideas on how to fix the issue, please share them:

```cpp
// Suggested fix or workaround
```

## Checklist

- [ ] I have searched existing issues to avoid duplicates
- [ ] I have provided all requested information
- [ ] I have included a minimal reproducible example (if possible)
- [ ] I have tested with the latest version of Elio
- [ ] I have included sanitizer output (for memory/threading issues)
