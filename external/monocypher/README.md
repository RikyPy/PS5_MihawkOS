# Monocypher's Ed25519 check

How MihawkOS checks a release's signature before it updates itself (`src/pw_release.c`, docs/SELF_INSTALL.md
phase 8). Monocypher 4.0.2, the release tarball `monocypher-4.0.2.tar.gz`, SHA-256
`38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c` (the same bytes from
monocypher.org/download and the GitHub release `4.0.2`), files unchanged:

- `monocypher.c`, `monocypher.h`: `src/`;
- `monocypher-ed25519.c`, `monocypher-ed25519.h`: `src/optional/`, Ed25519 as RFC 8032 has it (SHA-512), so a
  signature OpenSSL makes (`tools/sign-release.py`) is the one `crypto_ed25519_check` accepts;
- `LICENCE.md`: BSD-2-Clause or CC0-1.0; this project takes the BSD-2-Clause side.

The title uses `crypto_ed25519_check` alone. To check the copy:

```bash
curl -sSfLO https://monocypher.org/download/monocypher-4.0.2.tar.gz
echo '38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c  monocypher-4.0.2.tar.gz' | sha256sum -c
tar -xzf monocypher-4.0.2.tar.gz
cmp monocypher-4.0.2/src/optional/monocypher-ed25519.c external/monocypher/monocypher-ed25519.c
```
