package=openssl
$(package)_version=1.1.1w
$(package)_download_path=https://www.openssl.org/source/
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8

define $(package)_set_vars
  # Do NOT export ARFLAGS in config_env. The depends system never defines
  # $(package)_arflags, so it expands to empty string. When CFLAGS/CPPFLAGS
  # are passed as VAR=value (no positional flags), OpenSSL's Configure sets
  # $anyuseradd=false and falls back to reading env vars. An empty ARFLAGS
  # in the environment overrides the target default ("r"), producing a
  # Makefile with ARFLAGS= (empty), which causes "ar: two different
  # operation options specified" at build time.
  $(package)_config_env=AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)" CC="$($(package)_cc)"
  $(package)_config_env_android=ANDROID_NDK_ROOT=$(host_prefix)/native
  $(package)_config_opts=no-capieng no-dso no-dtls1 no-ec_nistp_64_gcc_128 no-gost
  $(package)_config_opts+=no-md2 no-rc5 no-rdrand no-rfc3779 no-sctp no-shared
  $(package)_config_opts+=no-ssl-trace no-ssl2 no-ssl3 no-tests no-unit-test no-weak-ssl-ciphers
  $(package)_config_opts+=no-zlib no-zlib-dynamic no-static-engine no-comp no-afalgeng
  $(package)_config_opts+=no-engine no-hw no-asm
  # Pass compiler flags as VAR=value assignments (per OpenSSL INSTALL docs)
  # rather than positional args, because multi-word flags like "-arch arm64"
  # get split by the shell and OpenSSL's Configure misparses the second word
  # as a target name, causing "target already defined" errors on ARM64 macOS.
  # Use $$ for deferred evaluation so OS-specific flags (cflags_linux, etc.)
  # appended by funcs.mk after set_vars runs are included in the expansion.
  $(package)_config_opts+=CFLAGS="$$($(package)_cflags)"
  $(package)_config_opts+=CPPFLAGS="$$($(package)_cppflags)"
  $(package)_cflags_linux=-fPIC
  $(package)_cppflags_linux=-D_GNU_SOURCE
  $(package)_cflags_freebsd=-fPIC
  $(package)_config_opts_x86_64_linux=linux-x86_64
  $(package)_config_opts_i686_linux=linux-generic32
  $(package)_config_opts_arm_linux=linux-generic32
  $(package)_config_opts_aarch64_linux=linux-aarch64
  $(package)_config_opts_riscv64_linux=linux-generic64
  $(package)_config_opts_mipsel_linux=linux-generic32
  $(package)_config_opts_x86_64_freebsd=BSD-x86_64
  $(package)_config_opts_x86_64_darwin=darwin64-x86_64-cc
  $(package)_config_opts_aarch64_darwin=darwin64-arm64-cc
  $(package)_config_opts_x86_64_mingw32=mingw64
  $(package)_config_opts_i686_mingw32=mingw
endef

define $(package)_preprocess_cmds
  sed -i.bak 's|define X509_CERT_FILE .*|define X509_CERT_FILE "/etc/ssl/certs/ca-certificates.crt"|' include/internal/cryptlib.h && \
  sed -i.bak 's|define X509_CERT_DIR .*|define X509_CERT_DIR "/etc/ssl/certs"|' include/internal/cryptlib.h
endef

define $(package)_config_cmds
  $($(package)_config_env) ./Configure $($(package)_config_opts) --prefix=$(host_prefix) --openssldir=/etc/ssl
endef

define $(package)_build_cmds
  $(MAKE) build_libs
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_dev
endef

define $(package)_postprocess_cmds
  rm -rf bin share lib/*.la
endef
