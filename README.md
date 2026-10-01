# ks3fs — an in-kernel S3 filesystem

`ks3fs` is a loadable Linux kernel module that mounts an S3-compatible bucket
(or a prefix inside one) as a filesystem, without FUSE. It speaks HTTP/1.1 and
AWS Signature V4 directly from the kernel over kernel TCP sockets. It is shipped
as a DKMS package and targets Ubuntu 24.04 (Noble) kernels: GA 6.8 and the HWE
6.11, 6.14, 6.17 and 7.0 series.

**Status: proof of concept.** It passes the regression suites below, but
see [Limitations](#limitations) before you trust it with data.

## Prior art

| Project | Kind | Notes |
|---|---|---|
| [s3fs-fuse](https://github.com/s3fs-fuse/s3fs-fuse) | FUSE | The classic POSIX-ish S3 mount. |
| [mountpoint-s3](https://github.com/awslabs/mountpoint-s3) | FUSE | AWS's high-throughput client. ks3fs follows its whole-object write model and its lack of chmod. Unlike mountpoint-s3, where directories shadow files, a file shadows a same-named prefix in ks3fs. |
| goofys, rclone mount, JuiceFS | FUSE | Various trade-offs; JuiceFS uses a separate metadata DB. |
| [liudangyi/httpfs](https://github.com/liudangyi/httpfs) | in-kernel | A toy HTTP/1.0 filesystem that downloads whole files on open. |
| fs/ceph + net/ceph, fs/smb, fs/nfs (mainline) | in-kernel | Network filesystem clients; these are the design references for socket handling, dcache priming (`nfs_prime_dcache`) and revalidation. |

No maintained in-kernel S3 filesystem was found.

## Design

```
 VFS ─ dir.c    lookup/readdir/create/mkdir/unlink/rmdir/rename/d_revalidate
     ─ file.c   page cache: ranged GET reads, buffered writes, PUT or
                streamed multipart upload
         │
       s3.c     HEAD / ListObjectsV2 / PUT / DELETE / CopyObject
       mpu.c    multipart upload, UploadPartCopy, large-object copy
       par.c    bounded parallel execution of independent requests
         │
       sigv4.c  canonical request + SigV4 (crypto API sha256, own HMAC)
       meta.c   x-amz-meta-* POSIX metadata (s3fs-fuse layout)
       http.c   HTTP/1.1 over kernel sockets, keep-alive pool, chunked,
                retries (soft/hard), signal-safe waits
       tls.c    TLS: net/handshake upcall to tlshd, then kTLS
       xml.c    minimal ListBucketResult/Error parsing
```

- **Namespace.** Directories are key prefixes ending in `/`. A directory exists
  if an explicit `dir/` marker object exists or if any key lies below the prefix.
  `mkdir` writes a marker and `rmdir` deletes one. If both `name` and `name/…`
  exist, the object `name` wins.
- **Reads** go through the page cache. Readahead issues one ranged GET per
  window (4 MiB by default). Every GET carries `If-Match: <etag>`, so the cache
  never mixes two versions of an object.
- **Writes** are buffered in the page cache. Partial overwrites
  read-modify-write from the store. `create` PUTs an empty object immediately,
  so a file is visible to other clients and permission errors appear at `open`.
  - A file smaller than two parts (`part_size`, default 16 MiB) is stored with
    one PUT on `close()`/`fsync()`.
  - Larger files use a multipart upload. Each completed part behind the write
    cursor is uploaded while the file is still being written, and its folios
    become clean and reclaimable. Dirty memory therefore stays at about one
    part per file however big the file gets, and the upload is mostly done by
    close time.
  - `close()`/`fsync()` completes the upload. Parts the writer never touched
    are copied server-side from the previous object version (UploadPartCopy,
    run in parallel), so appending to or patching a 6 GiB object doesn't
    resend it.
  - Uploaded parts are readable nowhere until completion, so they are frozen:
    a read or write that reaches one completes the upload first. `read_folio`
    returns `EIO` rather than filling such a range from the old object.
    Objects can be up to `part_size` × 10,000 (160 GiB by default, 5 TiB
    maximum).
- **Rename** of a file is a copy followed by DELETE; objects over 5 GiB use a
  multipart copy.
- **Directory rename** has no S3 equivalent, so it happens in stages:
  1. Pending metadata below the directory is stored first.
  2. Every object below the directory is copied to the new prefix, in
     parallel (`parallel=`, default 16). If a copy fails, the copies are
     deleted and the rename fails.
  3. Only then are the originals deleted.
  4. The key of every cached inode in the tree is rewritten, so open files
     keep working.

  If a file below is open with unsaved data, or the tree holds more than
  100,000 objects, the rename returns `EXDEV` and `mv` falls back to copying.
  The rename isn't atomic: other clients can see both trees while it runs.
- **Consistency.** Close-to-open. Dentries and attributes are trusted for
  `ttl` seconds (default 1). After that, the next path walk re-HEADs, and a
  changed ETag invalidates the cached pages. With `nometa`, `readdir`
  instantiates dentries from the listing, the way NFS READDIRPLUS does, so
  `ls -l` costs one LIST.
- **Metadata listings.** With metadata on, a listing can't say which objects
  are symlinks or what their modes are. Readdir therefore keeps its listing
  for 10 s. The first stat of one of its entries (`ls -l`, `find`) HEADs all
  of them in parallel and instantiates their dentries, so the rest of the
  stats are served from cache. At 20 ms latency, `ls -l` of 200 files takes
  1 s instead of 4 s.
- **POSIX metadata** (default; `nometa` turns it off). Mode (including the file
  type), uid, gid and mtime are stored as `x-amz-meta-mode/uid/gid/mtime`,
  the layout s3fs-fuse and rclone use, so buckets stay interoperable.
  - Symlinks are objects whose body is the target; `mode` says `S_IFLNK`.
  - chmod, chown and utimes are written back lazily by the VFS (`->write_inode`
    on writeback, `sync` or `fsync`) as one CopyObject onto the object itself,
    so a chmod followed by a utimes costs one request.
  - Directory modes live on the `dir/` marker object; one is created when an
    implicit directory is chmod-ed.
- **TLS** (`tls`). This is the NFS-over-TLS design. The kernel asks the
  `tlshd` agent (Ubuntu package `ktls-utils`), through the `net/handshake`
  netlink upcall, to run the TLS 1.3 handshake on its socket. `tlshd` verifies
  the server certificate against the system trust store and the `host=` name,
  then installs kTLS, after which the kernel sends and receives plaintext.
  Non-data records (TLS 1.3 session tickets, alerts) are handled through
  `TLS_GET_RECORD_TYPE` control messages.
- **Network failures.** Every request ks3fs issues is idempotent, so any
  request can be replayed.
  - Transport errors and 429/5xx responses are retried with exponential
    backoff (0.1 s doubling to 5 s).
  - `soft` (the default) gives up with `EIO` after `retry_timeout` seconds;
    `hard` retries until the process is killed.
  - A GET that breaks mid-body resumes with a ranged GET from the byte it had
    reached, still pinned to the same ETag. An upload that breaks restarts.
  - Socket waits block every signal except `SIGKILL`, as CIFS does, so a signal
    to the calling process can't become an I/O error while a hung server stays
    killable.
  - The kernel log reports `server … not responding, still trying` and later
    `server … OK`.
- **Compatibility.** `compat.h` covers the VFS API changes between 6.8 and 7.0:
  `write_begin` folio/kiocb, `d_revalidate`, `mkdir`, `set_default_d_op`,
  `inode_just_drop` and `sockaddr_unsized`.

## Usage

```sh
# MinIO / Ceph RGW / any endpoint, path-style
mount -t ks3fs -o endpoint=minio.lan:9000,credentials=/etc/ks3fs/creds mybucket/some/prefix /data

# public AWS bucket over HTTPS, anonymous, virtual-hosted style
mount -t ks3fs -o endpoint=https://s3.amazonaws.com,vhost,region=eu-west-1,ro nix-releases /mnt/nix
```

The kernel can't do DNS lookups and shouldn't see secrets in fstab. The
`mount.ks3fs` helper (installed to `/usr/sbin`) therefore resolves
`endpoint=` and reads `credentials=` (`access_key=`/`secret_key=` lines, or
AWS-style `aws_access_key_id =` lines) before calling mount(2). Raw kernel
options:

| option | meaning |
|---|---|
| `addr=` | server IPv4/IPv6 literal (required) |
| `port=` | default 80 |
| `host=` | Host header / signing name (default `addr[:port]`) |
| `vhost` | virtual-hosted addressing (`bucket.host`) |
| `region=` | SigV4 region, default `us-east-1` |
| `access_key=`, `secret_key=`, `session_token=` | credentials; omit for anonymous access. The secret is never shown in `/proc/mounts`. |
| `uid=`, `gid=`, `file_mode=`, `dir_mode=` | fixed ownership and permissions |
| `ttl=` | metadata cache lifetime in seconds (0 = revalidate on every lookup) |
| `timeout=` | socket timeout in seconds (default 30) |
| `tls` | HTTPS via tlshd + kTLS (port defaults to 443). `host=` must be the DNS name on the certificate. |
| `soft` / `hard` | give up with `EIO` after `retry_timeout`, or retry until killed (default `soft`) |
| `retry_timeout=` | soft mounts: seconds to keep retrying (default 60) |
| `nometa` | don't store POSIX metadata. Fixed modes, no symlinks, faster listings. |
| `part_size=` | multipart part size in MiB, 5–5120 (default 16). The largest object is 10,000 × this. |
| `parallel=` | concurrent requests for directory renames, prefetch and part copies (default 16) |

## Limitations

- TLS needs `tlshd` running. Noble's `tlshd` 0.9 only speaks TLS 1.3 and only
  verifies DNS names, not IP addresses. There is no way to skip certificate
  verification; add a private CA to the system trust store instead. Uploads
  use `UNSIGNED-PAYLOAD`, which is fine over TLS; over plain HTTP, rely on
  your network.
- Parts are streamed out as the write cursor passes them. Random writes all
  over a large file keep their dirty data in memory until close or fsync.
- An interrupted multipart upload (crash, power loss) leaves orphaned parts on
  the server. Use a bucket lifecycle rule (`AbortIncompleteMultipartUpload`)
  to clean them up.
- A reader using mmap on a file that another process is streaming out can get
  `EIO` for parts uploaded in between. `read()` and `splice()` complete the
  upload first instead.
- Shared writable `mmap` is refused (`EOPNOTSUPP`); read-only and private
  mmaps work.
- No hard links, special files or xattrs. Metadata changes reach other clients
  after writeback (about 5 s to 30 s) or `sync`.
- `O_DIRECT` is not supported.

## Building

```sh
make -C src                            # running kernel
make -C src KDIR=/path/to/headers CC=x86_64-linux-gnu-gcc-13
packaging/build-deb.sh                 # -> build/deb/ks3fs-dkms_<ver>_all.deb
```

`dkms.conf` builds `src/` for every installed kernel. The `.deb` registers the
module with DKMS on install and removes it on uninstall.

## Testing

**The module is only ever loaded inside QEMU/KVM guests.** `tests/run.sh
<kver>` does the following:

1. Downloads the stock Ubuntu kernel image and headers for `<kver>` (nothing
   is installed on the host) and builds the module with the compiler that
   kernel was built with. Warnings count as failures.
2. Starts an S3 server on the host (`S3_SERVER=minio|versitygw|rgw`) and seeds
   fixtures: odd key names, a 20 MB random object, 2,500 keys for pagination,
   implicit directories and marker directories.
3. Boots the kernel with a busybox initramfs that loads the module and runs
   the guest suites:
   - `rw` has 133 checks: reads, writes, append, in-place writes, truncate,
     rename, rmdir, chmod/chown/utimes, symlinks and `nometa`. Also a second
     mount observing changes after the TTL, and remounting. Also multipart
     uploads (streamed writes, append, an in-place write, reading an open
     file's uploaded parts) and directory rename (nested trees, open files, a
     move across parents, over an empty directory, `ENOTEMPTY`, `EXDEV` when
     busy, and 2,500 objects). And `ls -l` correctness on a fresh mount.
   - `rw-tls` runs the whole of `rw` again over TLS (on its own bucket).
   - `tls` has 22 checks:
     - the certificate is verified;
     - a wrong name or an untrusted CA is rejected;
     - plain HTTP to a TLS port fails;
     - a missing `tlshd` fails fast with a clear error;
     - real AWS works over HTTPS.
   - `faults` has 31 checks, driven through `tests/tools/faultproxy.py`, a
     proxy the guest controls:
     - connections cut mid-download and mid-upload, including mid-TLS-record;
     - a reset storm and an 8 s black hole;
     - refused connections;
     - the guest link going down;
     - a soft mount returning `EIO` in bounded time;
     - a hard mount waiting forever but still dying promptly on `SIGKILL`;
     - a read that stays correct under a 2 ms `SIGALRM` storm;
     - `ls -l` at 20 ms latency, which must be at least 3× faster with
       parallel prefetch.
   - `big` has 16 checks (opt-in; slow). It runs in a 1 GiB guest:
     - writes a 6 GiB object, reads it back and verifies the hash;
     - appends to it, which copies the 384 unchanged parts server-side;
     - chmods and renames it, both via multipart copy.
   - `nix` has 12 checks: anonymous reads from the public `nix-releases` AWS
     bucket, verified against its published `.sha256` files.
   - `nixstore` (opt-in; slow; needs internet):
     1. installs Nix from the official release tarball, read through ks3fs,
        into a ks3fs-backed `/nix/store`;
     2. substitutes a pinned stdenv (412 MiB, about 5,000 files) from
        cache.nixos.org;
     3. compiles GNU hello from source;
     4. runs `nix-store --verify --check-contents` before and after a remount.
        NAR hashes cover file contents, executable bits and symlinks.
4. Treats any kernel `BUG`/`WARNING`/KASAN/lockdep report as a failure; the
   guest also runs with `panic_on_warn`. The module must `rmmod` cleanly.
5. Verifies from the host, with the AWS CLI, what the guest wrote.

```sh
tests/run.sh 6.8.0-138-generic                  # one kernel, default suites
SUITES=nixstore tests/run.sh                    # Nix store on ks3fs
SUITES=big tests/run.sh                         # 6 GiB object in a 1 GiB guest
S3_SERVER=rgw SUITES=rw tests/run.sh            # against Ceph RGW
tests/matrix.sh                                 # every kernel in tests/kernels.txt
vm/build-debug-kernel.sh && tests/run.sh 6.8.0-debug   # Noble source + KASAN/lockdep/kmemleak/UBSAN
packaging/test-dkms.sh build/deb/*.deb $(tests/resolve-kernels.sh)
```

In `tests/kernels.txt`, `<series>-latest` entries resolve to the newest Ubuntu
ABI in the apt index, so the weekly CI run exercises each new Noble kernel
release when it lands.

MinIO no longer publishes binaries or images, and its repository was archived
in 2026. `tools/build-minio.sh` builds the last community release from source.
versitygw and Ceph RGW are independent implementations; running against all
three is what caught the servers that ignore `encoding-type=url`.

## CI

`.github/workflows/ci.yml` runs on `ubuntu-24.04` runners, which provide
`/dev/kvm`. It has these jobs:

- **kernels** resolves the kernel matrix.
- **lint** runs shellcheck.
- **build** compiles against each kernel's headers with warnings as errors.
- **vm-test** covers every kernel × {minio, versitygw, rgw} in KVM guests,
  with console logs uploaded as artifacts.
- **vm-test-sanitizers** runs the Noble 6.8 source rebuilt with KASAN,
  lockdep, kmemleak and UBSAN (the kernel build is cached).
- **vm-test-nixstore** runs the Nix store suite on the newest GA and HWE
  kernels.
- **dkms** installs the `.deb` into a clean Noble container with headers for
  every matrix kernel.

A weekly schedule picks up new kernel ABIs.

## Roadmap

1. DNS via the `dns_resolver` key type as an alternative to the mount helper.
   Client certificates (mutual TLS) via `tls_client_hello_x509` and the keyring.
2. Credentials from the kernel keyring, and refreshable session tokens.
3. Run xfstests (the `generic/` subset that applies) and `fsx` in the guest.
4. Large folios, and parallel ranged GETs for sequential reads.
5. Mainline-kernel build job to catch VFS API drift early.
6. Background (workqueue) part uploads, so a writer isn't paused while a
   part goes out.
