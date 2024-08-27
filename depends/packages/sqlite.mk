package=sqlite
$(package)_version=3450200
$(package)_download_path=https://sqlite.org/2024/
$(package)_file_name=sqlite-autoconf-$($(package)_version).tar.gz
$(package)_sha256_hash=bc9067442eedf3dd39989b5c5cfbfff37ae66cc9c99274e0c3052dc4d4a8f6ae

define $(package)_set_vars
$(package)_config_opts=--disable-shared --disable-readline --disable-dynamic-extensions --enable-option-checking
$(package)_config_opts_linux=--with-pic
endef

define $(package)_preprocess_cmds
  cp -f $(BASEDIR)/config.guess $(BASEDIR)/config.sub .
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE) libsqlite3.la
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install-libLTLIBRARIES install-includeHEADERS install-pkgconfigDATA
endef

define $(package)_postprocess_cmds
  rm lib/*.la
endef
