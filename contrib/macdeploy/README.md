# MacOS Deployment

The `macdeployqtplus` script should not be run manually. Instead, after building as usual:

```bash
make deploy
```

When complete, it will have produced `DigiByte-Core.zip`.

## Check the release package on a Mac

The Linux release build produces an **unsigned** app bundle. The executable
may contain a basic signature added by the linker. That does not sign the
whole app, including its settings and resources. Changing the ZIP filename
does not complete this step.

1. Finish signing the whole `DigiByte-Qt.app` after all its files are in place.
   Use the detached-signature process below for Developer ID signing. Native
   `make deploy` uses a local, or *ad hoc*, bundle signature. That local
   signature checks file integrity; it does not identify a trusted publisher
   or notarize the app with Apple.
2. On a Mac, extract the exact ZIP intended for publication into a new folder.
   Run these checks from that folder:

   ```bash
   test -f DigiByte-Qt.app/Contents/_CodeSignature/CodeResources
   codesign --verify --deep --strict --verbose=2 DigiByte-Qt.app
   codesign --display --verbose=4 DigiByte-Qt.app
   DigiByte-Qt.app/Contents/MacOS/DigiByte-Qt -version
   ```

3. Check that the displayed version and architecture match the release.
   Test the final downloaded ZIP on its target Mac. Confirm that the wallet
   opens through the normal macOS approval process. Use a separate test data
   directory and test wallet. Do not use a live wallet for this check.
4. Record the ZIP checksum, macOS version, signature checks and launch result.
   Generate the published checksums after signing and packaging are complete.

A passing Linux build or checksum comparison does not replace these Mac
checks. If a signature check or launch fails, keep that package out of the
release until the cause is fixed. Do not disable Gatekeeper to make the check
pass. See Apple's [code signing checks](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/Procedures/Procedures.html).

## SDK Extraction

### Step 1: Obtaining `Xcode.app`

A free Apple Developer Account is required to proceed.

Our current macOS SDK
(`Xcode-12.2-12B45b-extracted-SDK-with-libcxx-headers.tar.gz`)
can be extracted from
[Xcode_12.2.xip](https://download.developer.apple.com/Developer_Tools/Xcode_12.2/Xcode_12.2.xip).

Alternatively, after logging in to your account go to 'Downloads', then 'More'
and search for [`Xcode 12.2`](https://developer.apple.com/download/all/?q=Xcode%2012.2).

An Apple ID and cookies enabled for the hostname are needed to download this.

The `sha256sum` of the downloaded XIP archive should be `28d352f8c14a43d9b8a082ac6338dc173cb153f964c6e8fb6ba389e5be528bd0`.

After Xcode version 7.x, Apple started shipping the `Xcode.app` in a `.xip`
archive. This makes the SDK less-trivial to extract on non-macOS machines. One
approach (tested on Debian Buster) is outlined below:

```bash
# Install/clone tools needed for extracting Xcode.app
apt install cpio
git clone https://github.com/digibyte-core/apple-sdk-tools.git

# Unpack Xcode_12.2.xip and place the resulting Xcode.app in your current
# working directory
python3 apple-sdk-tools/extract_xcode.py -f Xcode_12.2.xip | cpio -d -i
```

On macOS the process is more straightforward:

```bash
xip -x Xcode_12.2.xip
```

### Step 2: Generating `Xcode-12.2-12B45b-extracted-SDK-with-libcxx-headers.tar.gz` from `Xcode.app`

To generate `Xcode-12.2-12B45b-extracted-SDK-with-libcxx-headers.tar.gz`, run
the script [`gen-sdk`](./gen-sdk) with the path to `Xcode.app` (extracted in the
previous stage) as the first argument.

```bash
# Generate a Xcode-12.2-12B45b-extracted-SDK-with-libcxx-headers.tar.gz from
# the supplied Xcode.app
./contrib/macdeploy/gen-sdk '/path/to/Xcode.app'
```

The `sha256sum` of the generated TAR.GZ archive should be `df75d30ecafc429e905134333aeae56ac65fac67cb4182622398fd717df77619`.

## Deterministic macOS App Notes

macOS Applications are created in Linux by combining a recent `clang` and the Apple
`binutils` (`ld`, `ar`, etc).

Apple uses `clang` extensively for development and has upstreamed the necessary
functionality so that a vanilla clang can take advantage. It supports the use of `-F`,
`-target`, `-mmacosx-version-min`, and `-isysroot`, which are all necessary when
building for macOS.

Apple's version of `binutils` (called `cctools`) contains lots of functionality missing in the
FSF's `binutils`. In addition to extra linker options for frameworks and sysroots, several
other tools are needed as well such as `install_name_tool`, `lipo`, and `nmedit`. These
do not build under Linux, so they have been patched to do so. The work here was used as
a starting point: [mingwandroid/toolchain4](https://github.com/mingwandroid/toolchain4).

In order to build a working toolchain, the following source packages are needed from
Apple: `cctools`, `dyld`, and `ld64`.

These tools inject timestamps by default, which produce non-deterministic binaries. The
`ZERO_AR_DATE` environment variable is used to disable that.

This version of `cctools` has been patched to use the current version of `clang`'s headers
and its `libLTO.so` rather than those from `llvmgcc`, as it was originally done in `toolchain4`.

To complicate things further, all builds must target an Apple SDK. These SDKs are free to
download, but not redistributable. See the SDK Extraction notes above for how to obtain it.

The Guix process builds 2 sets of files: Linux tools, then Apple binaries which are
created using these tools. The build process has been designed to avoid including the
SDK's files in Guix's outputs. All interim tarballs are fully deterministic and may be freely
redistributed.

As of OS X 10.9 Mavericks, using an Apple-blessed key to sign binaries is a requirement in
order to satisfy the new Gatekeeper requirements. Because this private key cannot be
shared, we'll have to be a bit creative in order for the build process to remain somewhat
deterministic. Here's how it works:

- Builders use Guix to create an unsigned release. This outputs an unsigned ZIP which
  users may choose to bless and run. It also outputs an unsigned app structure in the form
  of a tarball.
- The Apple keyholder uses this unsigned app to create a detached signature, using the
  script that is also included there. Detached signatures are available from this [repository](https://github.com/digibyte-core/digibyte-detached-sigs).
- Builders feed the unsigned app + detached signature back into Guix. It uses the
  pre-built tools to recombine the pieces into a deterministic ZIP.
