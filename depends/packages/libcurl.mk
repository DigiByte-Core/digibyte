package=libcurl
$(package)_version=8.5.0
$(package)_download_path=https://curl.se/download/
$(package)_file_name=curl-$($(package)_version).tar.gz
$(package)_sha256_hash=05fc17ff25b793a437a0906e0484b82172a9f4de02be5ed447e0cab8c3475add

$(package)_dependencies=openssl

define $(package)_set_vars
  $(package)_config_opts = --disable-shared --enable-static --with-openssl=$(host_prefix)
  $(package)_config_opts += --disable-manual --disable-ldap --disable-ldaps
  $(package)_config_opts += --without-librtmp --disable-dict --disable-file --disable-ftp
  $(package)_config_opts += --disable-gopher --disable-imap --disable-mqtt --disable-pop3
  $(package)_config_opts += --disable-rtsp --disable-smb --disable-smtp --disable-telnet
  $(package)_config_opts += --disable-tftp --without-brotli --without-zstd --without-libidn2
  $(package)_config_opts += --without-libpsl --without-nghttp2 --disable-dependency-tracking
  $(package)_config_opts_linux=--with-pic
  # -D_GNU_SOURCE exposes POSIX functions (fileno, fdopen) that libcurl's
  # fopen.c needs but are hidden by the depends system's strict -std=c11.
  $(package)_cppflags_linux=-D_GNU_SOURCE
  $(package)_config_env_linux=LIBS="-ldl -lpthread"
  $(package)_config_opts_mingw32=--with-pic
  $(package)_config_env_mingw32=LIBS="-lws2_32 -lcrypt32"
endef

define $(package)_config_cmds
  cp -f $(BASEDIR)/config.guess $(BASEDIR)/config.sub . && \
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf bin share lib/*.la
endef
