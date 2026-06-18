---
name: Feature request
about: Suggest an idea for Elio
title: '[Feature] '
labels: 'enhancement'
assignees: ''
---

## Feature Description

A clear and concise description of the feature you'd like to see.

## Problem Statement

**Is your feature request related to a problem? Please describe.**

A clear and concise description of what the problem is. Ex. I'm always frustrated when [...]

## Proposed Solution

**Describe the solution you'd like**

A clear and concise description of what you want to happen.

### API Design

If applicable, provide a sketch of the proposed API:

```cpp
// Example of how the feature would be used
namespace elio {

// Your proposed API here
class new_feature {
public:
    // ...
};

} // namespace elio
```

### Usage Example

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<void> example_usage() {
    // How users would use this feature
    co_return;
}
```

## Alternatives Considered

**Describe alternatives you've considered**

A clear and concise description of any alternative solutions or features you've considered.

### Alternative 1
[Description]

### Alternative 2
[Description]

## Use Cases

**Who would benefit from this feature? What are the primary use cases?**

1. [Use case 1]
2. [Use case 2]
3. [Use case 3]

## Implementation Considerations

If you have thoughts on how this could be implemented:

- **Complexity**: [Low/Medium/High]
- **Breaking Changes**: [Yes/No]
- **Dependencies**: [Any new dependencies required?]
- **Performance Impact**: [Expected performance characteristics]

## Compatibility

- **C++ Standard**: [Does this require C++23 or newer?]
- **Platform**: [Linux-only or cross-platform?]
- **Backward Compatibility**: [How would this affect existing code?]

## Related Work

**Are there similar features in other libraries or languages?**

- [Library/Language 1]: [How they handle this]
- [Library/Language 2]: [How they handle this]

## Additional Context

Add any other context, screenshots, diagrams, or references about the feature request here.

## Checklist

- [ ] I have searched existing issues to avoid duplicates
- [ ] I have clearly described the problem and proposed solution
- [ ] I have considered alternative approaches
- [ ] I have provided usage examples
- [ ] I have considered backward compatibility

## Priority

How important is this feature to you?

- [ ] **Critical**: Blocking my project, need it ASAP
- [ ] **High**: Would significantly improve my workflow
- [ ] **Medium**: Nice to have, but not urgent
- [ ] **Low**: Just an idea, no rush
