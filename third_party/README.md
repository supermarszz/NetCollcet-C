# Third-Party Dependencies

Dependency source trees are downloaded by `setup.ps1` / `bootstrap.ps1` and are not committed. The pinned archive URLs, versions, extraction paths and SHA-256 checksums are in `scripts/dependencies.json`.

Libraries: libssh2 1.11.1, Expat 2.7.1, miniz 3.0.2 and libtelnet 0.23. See `THIRD_PARTY_NOTICES.md` for license information. `package.ps1` copies the upstream licenses into the Windows release archive.
