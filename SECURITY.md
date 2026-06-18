# Security Policy

## Supported Versions

Elio follows semantic versioning. We provide security updates for the following versions:

| Version | Supported          |
| ------- | ------------------ |
| 0.5.x   | :white_check_mark: |
| 0.4.x   | :white_check_mark: |
| < 0.4   | :x:                |

## Reporting a Vulnerability

We take security vulnerabilities seriously. If you discover a security issue in Elio, please report it responsibly.

### How to Report

**Please do NOT open a public GitHub issue for security vulnerabilities.**

Instead, please report security issues via email:

- **Email**: [coldwings@me.com](mailto:coldwings@me.com)
- **Subject**: `[SECURITY] Elio: <brief description>`
- **Encryption**: You can use our PGP key (available upon request) for sensitive communications

### What to Include

Please include the following information in your report:

1. **Description**: Clear description of the vulnerability
2. **Impact**: Potential impact and affected components
3. **Reproduction**: Steps to reproduce the issue (if applicable)
4. **Proof of Concept**: Code or demonstration (if available)
5. **Suggested Fix**: Any ideas for mitigating the vulnerability (optional)

### Response Timeline

- **Acknowledgment**: Within 48 hours of receipt
- **Initial Assessment**: Within 5 business days
- **Status Update**: Weekly updates on progress
- **Resolution**: Target within 30 days for critical issues

### Disclosure Policy

We follow a coordinated disclosure process:

1. We acknowledge receipt of your report
2. We investigate and validate the vulnerability
3. We develop and test a fix
4. We release a security patch and notify affected users
5. We publicly disclose the vulnerability after users have had time to update (typically 7-14 days after patch release)

### Recognition

We appreciate responsible security research and will:

- Credit reporters in security advisories (unless anonymity is requested)
- Add reporters to our [SECURITY-THANKS.md](SECURITY-THANKS.md) file
- Provide reference letters for significant contributions (upon request)

## Security Best Practices for Users

When using Elio in production:

1. **Keep Updated**: Always use the latest supported version
2. **Monitor Advisories**: Watch this repository for security advisories
3. **Review Dependencies**: Regularly audit dependencies for known vulnerabilities
4. **Secure Configuration**: Follow our [security guidelines](wiki/Security-Guidelines.md)
5. **Input Validation**: Validate all inputs when using Elio in security-sensitive contexts

## Security Features

Elio includes several security-focused features:

- **Memory Safety**: TSAN and ASAN testing in CI
- **Thread Safety**: Comprehensive TSAN coverage for concurrent operations
- **Input Validation**: Robust parsing in HTTP/WebSocket components
- **Cryptographic Support**: OpenSSL integration with proper certificate validation
- **Resource Limits**: Configurable limits to prevent DoS attacks

## Scope

This security policy applies to:

- Elio core library (header-only)
- Official examples and tests
- Documentation

It does **not** apply to:

- Third-party dependencies (report to their respective maintainers)
- User applications built with Elio
- Unofficial forks or modifications

## Past Security Advisories

See our [Security Advisories](https://github.com/Coldwings/Elio/security/advisories) page for a history of disclosed vulnerabilities and their fixes.

## Contact

For general security questions (not vulnerability reports):

- **Email**: [coldwings@me.com](mailto:coldwings@me.com)
- **GitHub Discussions**: [Security category](https://github.com/Coldwings/Elio/discussions/categories/security)

---

**Last Updated**: 2026-06-18
