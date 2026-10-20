# Zstandard's single-file decoder

The decoder the title installs its runtime with (`src/pw_pack.c`). Zstandard v1.5.7 (tag `v1.5.7`, commit
`f8745da6ff1ad1e7bab384bd1f9d742439278e99`), decompression only:

- `zstddeclib.c`: made by the release's own `build/single_file_libs/create_single_file_decoder.sh`, unchanged
  (legacy formats and error strings left out, no assembly);
- `zstd.h`, `zstd_errors.h`: `lib/` of the same revision, unchanged;
- `LICENSE`: the BSD licence, the one of its two (BSD or GPL-2.0) this project takes.

To check or remake it:

```bash
git -C <a zstd checkout> archive v1.5.7 | tar -x -C /tmp/zstd-1.5.7
(cd /tmp/zstd-1.5.7/build/single_file_libs && sh create_single_file_decoder.sh)
cmp /tmp/zstd-1.5.7/build/single_file_libs/zstddeclib.c external/zstd/zstddeclib.c
```
