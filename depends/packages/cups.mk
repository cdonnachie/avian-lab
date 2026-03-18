package=cups

$(package)_version=2.4.7
$(package)_download_path=https://github.com/OpenPrinting/cups/releases/download/v$($(package)_version)
$(package)_file_name=cups-$($(package)_version)-source.tar.gz
$(package)_sha256_hash=dd54228dd903526428ce7e37961afaed230ad310788141da75cebaa08362cf6c

$(package)_dependencies =

define $(package)_preprocess_cmds
	sed -i '' 's|#ifdef HAVE_OPENSSL|#if 0|g' cups/hash.c && \
	sed -i '' 's|#else // HAVE_GNUTLS|#elif 0|g' cups/hash.c
endef

define $(package)_config_cmds
	./configure \
	  --prefix=$(host_prefix) \
	  --exec-prefix=$(host_prefix) \
	  --libdir=$(host_prefix)/lib \
	  --includedir=$(host_prefix)/include \
	  --with-pkgconfpath=$(host_prefix)/lib/pkgconfig \
	  --host=$(host) \
	  --disable-shared \
	  --enable-static \
	  --disable-dbus \
	  --disable-libpaper \
	  --disable-pam \
	  --disable-gssapi \
	  --disable-avahi \
	  --without-systemd \
	  --with-components=libcups \
	  --with-tls=no
endef

define $(package)_build_cmds
	$(MAKE)
endef

define $(package)_stage_cmds
	$(MAKE) DESTDIR=$($(package)_staging_dir) install
endef
