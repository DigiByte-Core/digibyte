# Copyright (c) 2009-2022 The Bitcoin Core developers
# Copyright (c) 2014-2024 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Pattern rule to print variables, e.g. make print-top_srcdir
print-%: FORCE
	@echo '$*'='$($*)'

DIST_SUBDIRS = secp256k1

AM_LDFLAGS = $(LIBTOOL_LDFLAGS) $(HARDENED_LDFLAGS) $(GPROF_LDFLAGS) $(SANITIZER_LDFLAGS) $(LTO_LDFLAGS) $(CORE_LDFLAGS)
AM_CXXFLAGS = $(DEBUG_CXXFLAGS) $(HARDENED_CXXFLAGS) $(WARN_CXXFLAGS) $(NOWARN_CXXFLAGS) $(ERROR_CXXFLAGS) $(GPROF_CXXFLAGS) $(SANITIZER_CXXFLAGS) $(LTO_CXXFLAGS) $(CORE_CXXFLAGS)
AM_CPPFLAGS = $(DEBUG_CPPFLAGS) $(HARDENED_CPPFLAGS) $(CORE_CPPFLAGS)
AM_LIBTOOLFLAGS = --preserve-dup-deps
PTHREAD_FLAGS = $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)
EXTRA_LIBRARIES =

lib_LTLIBRARIES =
noinst_LTLIBRARIES =

bin_PROGRAMS =
noinst_PROGRAMS =
check_PROGRAMS =
TESTS =
BENCHMARKS =

DIGIBYTE_INCLUDES=-I$(builddir) -I$(srcdir)/$(MINISKETCH_INCLUDE_DIR_INT) -I$(srcdir)/secp256k1/include -I$(srcdir)/$(UNIVALUE_INCLUDE_DIR_INT)

LIBDIGIBYTE_NODE=libdigibyte_node.a
LIBDIGIBYTE_COMMON=libdigibyte_common.a
LIBDIGIBYTE_CONSENSUS=libdigibyte_consensus.a
LIBDIGIBYTE_CLI=libdigibyte_cli.a
LIBDIGIBYTE_UTIL=libdigibyte_util.a
LIBDIGIBYTE_CRYPTO_BASE=crypto/libdigibyte_crypto_base.la
LIBDIGIBYTEQT=qt/libdigibyteqt.a
LIBSECP256K1=secp256k1/libsecp256k1.la

if ENABLE_ZMQ
LIBDIGIBYTE_ZMQ=libdigibyte_zmq.a
endif
if BUILD_DIGIBYTE_LIBS
LIBDIGIBYTECONSENSUS=libdigibyteconsensus.la
endif
if BUILD_DIGIBYTE_KERNEL_LIB
LIBDIGIBYTEKERNEL=libdigibytekernel.la
endif
if ENABLE_WALLET
LIBDIGIBYTE_WALLET=libdigibyte_wallet.a
LIBDIGIBYTE_WALLET_TOOL=libdigibyte_wallet_tool.a
endif

LIBDIGIBYTE_CRYPTO = $(LIBDIGIBYTE_CRYPTO_BASE)
if ENABLE_SSE41
LIBDIGIBYTE_CRYPTO_SSE41 = crypto/libdigibyte_crypto_sse41.la
LIBDIGIBYTE_CRYPTO += $(LIBDIGIBYTE_CRYPTO_SSE41)
endif
if ENABLE_AVX2
LIBDIGIBYTE_CRYPTO_AVX2 = crypto/libdigibyte_crypto_avx2.la
LIBDIGIBYTE_CRYPTO += $(LIBDIGIBYTE_CRYPTO_AVX2)
endif
if ENABLE_X86_SHANI
LIBDIGIBYTE_CRYPTO_X86_SHANI = crypto/libdigibyte_crypto_x86_shani.la
LIBDIGIBYTE_CRYPTO += $(LIBDIGIBYTE_CRYPTO_X86_SHANI)
endif
if ENABLE_ARM_SHANI
LIBDIGIBYTE_CRYPTO_ARM_SHANI = crypto/libdigibyte_crypto_arm_shani.la
LIBDIGIBYTE_CRYPTO += $(LIBDIGIBYTE_CRYPTO_ARM_SHANI)
endif
noinst_LTLIBRARIES += $(LIBDIGIBYTE_CRYPTO)

$(LIBSECP256K1): $(wildcard secp256k1/src/*.h) $(wildcard secp256k1/src/*.c) $(wildcard secp256k1/include/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C $(@D) $(@F)

# Make is not made aware of per-object dependencies to avoid limiting building parallelization
# But to build the less dependent modules first, we manually select their order here:
EXTRA_LIBRARIES += \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_CONSENSUS) \
  $(LIBDIGIBYTE_NODE) \
  $(LIBDIGIBYTE_CLI) \
  $(LIBDIGIBYTE_IPC) \
  $(LIBDIGIBYTE_WALLET) \
  $(LIBDIGIBYTE_WALLET_TOOL) \
  $(LIBDIGIBYTE_ZMQ)

if BUILD_DIGIBYTED
  bin_PROGRAMS += digibyted
endif

if BUILD_DIGIBYTE_NODE
  bin_PROGRAMS += digibyte-node
endif

if BUILD_DIGIBYTE_CLI
  bin_PROGRAMS += digibyte-cli
endif

if BUILD_DIGIBYTE_TX
  bin_PROGRAMS += digibyte-tx
endif

if ENABLE_WALLET
if BUILD_DIGIBYTE_WALLET
  bin_PROGRAMS += digibyte-wallet
endif
endif

if BUILD_DIGIBYTE_UTIL
  bin_PROGRAMS += digibyte-util
endif

if BUILD_DIGIBYTE_CHAINSTATE
  bin_PROGRAMS += digibyte-chainstate
endif

.PHONY: FORCE check-symbols check-security
# digibyte core #
DIGIBYTE_CORE_H = \
  addresstype.h \
  addrdb.h \
  addrman.h \
  addrman_impl.h \
  attributes.h \
  banman.h \
  base58.h \
  bech32.h \
  bip324.h \
  blockencodings.h \
  blockfilter.h \
  chain.h \
  chainparams.h \
  chainparamsbase.h \
  chainparamsseeds.h \
  checkqueue.h \
  clientversion.h \
  coins.h \
  common/args.h \
  common/bloom.h \
  common/init.h \
  common/run_command.h \
  common/url.h \
  compat/assumptions.h \
  compat/byteswap.h \
  compat/compat.h \
  compat/cpuid.h \
  compat/endian.h \
  common/settings.h \
  common/system.h \
  compressor.h \
  consensus/consensus.h \
  consensus/tx_check.h \
  consensus/tx_verify.h \
  core_io.h \
  core_memusage.h \
  cuckoocache.h \
  dandelion.h \
  dbwrapper.h \
  deploymentinfo.h \
  deploymentstatus.h \
  digidollar/digidollar.h \
  digidollar/scripts.h \
  digidollar/txbuilder.h \
  digidollar/validation.h \
  external_signer.h \
  flatfile.h \
  headerssync.h \
  httprpc.h \
  httpserver.h \
  i2p.h \
  index/base.h \
  index/blockfilterindex.h \
  index/coinstatsindex.h \
  index/digidollarstatsindex.h \
  index/disktxpos.h \
  index/txindex.h \
  indirectmap.h \
  init.h \
  init/common.h \
  interfaces/chain.h \
  interfaces/echo.h \
  interfaces/handler.h \
  interfaces/init.h \
  interfaces/ipc.h \
  interfaces/node.h \
  interfaces/wallet.h \
  kernel/blockmanager_opts.h \
  kernel/chain.h \
  kernel/chainparams.h \
  kernel/chainstatemanager_opts.h \
  kernel/checks.h \
  kernel/coinstats.h \
  kernel/context.h \
  kernel/cs_main.h \
  kernel/disconnected_transactions.h \
  kernel/mempool_entry.h \
  kernel/mempool_limits.h \
  kernel/mempool_options.h \
  kernel/mempool_persist.h \
  kernel/mempool_removal_reason.h \
  kernel/messagestartchars.h \
  kernel/notifications_interface.h \
  kernel/validation_cache_sizes.h \
  key.h \
  key_io.h \
  logging.h \
  logging/timer.h \
  mapport.h \
  memusage.h \
  merkleblock.h \
  net.h \
  net_permissions.h \
  net_processing.h \
  net_types.h \
  netaddress.h \
  netbase.h \
  netgroup.h \
  netmessagemaker.h \
  node/abort.h \
  node/blockmanager_args.h \
  node/blockstorage.h \
  node/caches.h \
  node/chainstate.h \
  node/chainstatemanager_args.h \
  node/coin.h \
  node/coins_view_args.h \
  node/connection_types.h \
  node/context.h \
  node/database_args.h \
  node/eviction.h \
  node/interface_ui.h \
  node/kernel_notifications.h \
  node/mempool_args.h \
  node/mempool_persist_args.h \
  node/miner.h \
  node/mini_miner.h \
  node/minisketchwrapper.h \
  node/peerman_args.h \
  node/psbt.h \
  node/transaction.h \
  node/txreconciliation.h \
  node/utxo_snapshot.h \
  node/validation_cache_args.h \
  noui.h \
  outputtype.h \
  policy/feerate.h \
  policy/fees.h \
  policy/fees_args.h \
  policy/packages.h \
  policy/policy.h \
  policy/rbf.h \
  policy/settings.h \
  pow.h \
  protocol.h \
  psbt.h \
  random.h \
  randomenv.h \
  rest.h \
  reverse_iterator.h \
  rpc/blockchain.h \
  rpc/client.h \
  rpc/mempool.h \
  rpc/mining.h \
  rpc/net.h \
  rpc/protocol.h \
  rpc/rawtransaction_util.h \
  rpc/register.h \
  rpc/request.h \
  rpc/server.h \
  rpc/server_util.h \
  rpc/util.h \
  scheduler.h \
  script/descriptor.h \
  script/keyorigin.h \
  script/miniscript.h \
  script/sigcache.h \
  script/sign.h \
  script/signingprovider.h \
  script/solver.h \
  shutdown.h \
  signet.h \
  streams.h \
  support/allocators/pool.h \
  support/allocators/secure.h \
  support/allocators/zeroafterfree.h \
  support/cleanse.h \
  support/events.h \
  support/lockedpool.h \
  sync.h \
  threadsafety.h \
  timedata.h \
  torcontrol.h \
  txdb.h \
  txmempool.h \
  txorphanage.h \
  txrequest.h \
  undo.h \
  util/any.h \
  util/asmap.h \
  util/batchpriority.h \
  util/bip32.h \
  util/bitdeque.h \
  util/bytevectorhash.h \
  util/chaintype.h \
  util/check.h \
  util/epochguard.h \
  util/error.h \
  util/exception.h \
  util/fastrange.h \
  util/fees.h \
  util/fs.h \
  util/fs_helpers.h \
  util/getuniquepath.h \
  util/golombrice.h \
  util/hash_type.h \
  util/hasher.h \
  util/insert.h \
  util/macros.h \
  util/message.h \
  util/moneystr.h \
  util/overflow.h \
  util/overloaded.h \
  util/rbf.h \
  util/readwritefile.h \
  util/result.h \
  util/serfloat.h \
  util/signalinterrupt.h \
  util/sock.h \
  util/spanparsing.h \
  util/string.h \
  util/syserror.h \
  util/thread.h \
  util/threadinterrupt.h \
  util/threadnames.h \
  util/time.h \
  util/tokenpipe.h \
  util/trace.h \
  util/translation.h \
  util/types.h \
  util/ui_change_type.h \
  util/vector.h \
  validation.h \
  validationinterface.h \
  versionbits.h \
  wallet/bdb.h \
  wallet/coincontrol.h \
  wallet/coinselection.h \
  wallet/context.h \
  wallet/crypter.h \
  wallet/db.h \
  wallet/digidollarwallet.h \
  wallet/ddcoincontrol.h \
  wallet/dump.h \
  wallet/external_signer_scriptpubkeyman.h \
  wallet/feebumper.h \
  wallet/fees.h \
  wallet/load.h \
  wallet/receive.h \
  wallet/rpc/util.h \
  wallet/rpc/wallet.h \
  wallet/salvage.h \
  wallet/scriptpubkeyman.h \
  wallet/spend.h \
  wallet/sqlite.h \
  wallet/transaction.h \
  wallet/types.h \
  wallet/wallet.h \
  wallet/walletdb.h \
  wallet/wallettool.h \
  wallet/walletutil.h \
  walletinitinterface.h \
  warnings.h \
  zmq/zmqabstractnotifier.h \
  zmq/zmqnotificationinterface.h \
  zmq/zmqpublishnotifier.h \
  zmq/zmqrpc.h \
  zmq/zmqutil.h


obj/build.h: FORCE
	@$(MKDIR_P) $(builddir)/obj
	$(AM_V_GEN) $(top_srcdir)/share/genbuild.sh "$(abs_top_builddir)/src/obj/build.h" \
	  "$(abs_top_srcdir)"
libdigibyte_util_a-clientversion.$(OBJEXT): obj/build.h

# node #
libdigibyte_node_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(LEVELDB_CPPFLAGS) $(BOOST_CPPFLAGS) $(MINIUPNPC_CPPFLAGS) $(NATPMP_CPPFLAGS) $(EVENT_CFLAGS) $(EVENT_PTHREADS_CFLAGS) $(LIBCURL_CFLAGS)
libdigibyte_node_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_node_a_SOURCES = \
  addrdb.cpp \
  addrman.cpp \
  banman.cpp \
  bip324.cpp \
  blockencodings.cpp \
  blockfilter.cpp \
  chain.cpp \
  consensus/tx_verify.cpp \
  dandelion.cpp \
  dbwrapper.cpp \
  deploymentstatus.cpp \
  digidollar/health.cpp \
  flatfile.cpp \
  headerssync.cpp \
  httprpc.cpp \
  httpserver.cpp \
  i2p.cpp \
  index/base.cpp \
  index/blockfilterindex.cpp \
  index/coinstatsindex.cpp \
  index/digidollarstatsindex.cpp \
  index/txindex.cpp \
  init.cpp \
  kernel/chain.cpp \
  kernel/checks.cpp \
  kernel/coinstats.cpp \
  kernel/context.cpp \
  kernel/cs_main.cpp \
  kernel/mempool_persist.cpp \
  kernel/mempool_removal_reason.cpp \
  mapport.cpp \
  net.cpp \
  net_processing.cpp \
  netgroup.cpp \
  node/abort.cpp \
  node/blockmanager_args.cpp \
  node/blockstorage.cpp \
  node/caches.cpp \
  node/chainstate.cpp \
  node/chainstatemanager_args.cpp \
  node/coin.cpp \
  node/coins_view_args.cpp \
  node/connection_types.cpp \
  node/context.cpp \
  node/database_args.cpp \
  node/eviction.cpp \
  node/interface_ui.cpp \
  node/interfaces.cpp \
  node/kernel_notifications.cpp \
  node/mempool_args.cpp \
  node/mempool_persist_args.cpp \
  node/miner.cpp \
  node/mini_miner.cpp \
  node/minisketchwrapper.cpp \
  node/peerman_args.cpp \
  node/psbt.cpp \
  node/transaction.cpp \
  node/txreconciliation.cpp \
  node/utxo_snapshot.cpp \
  node/validation_cache_args.cpp \
  noui.cpp \
  policy/fees.cpp \
  policy/fees_args.cpp \
  policy/packages.cpp \
  policy/rbf.cpp \
  policy/settings.cpp \
  pow.cpp \
  rest.cpp \
  rpc/blockchain.cpp \
  rpc/digidollar.cpp \
  rpc/fees.cpp \
  rpc/mempool.cpp \
  rpc/mining.cpp \
  rpc/net.cpp \
  rpc/node.cpp \
  rpc/output_script.cpp \
  rpc/rawtransaction.cpp \
  rpc/server.cpp \
  rpc/server_util.cpp \
  rpc/signmessage.cpp \
  rpc/txoutproof.cpp \
  script/sigcache.cpp \
  shutdown.cpp \
  signet.cpp \
  timedata.cpp \
  torcontrol.cpp \
  txdb.cpp \
  txmempool.cpp \
  txorphanage.cpp \
  txrequest.cpp \
  validation.cpp \
  validationinterface.cpp \
  versionbits.cpp \
  $(DIGIBYTE_CORE_H)

if ENABLE_WALLET
libdigibyte_node_a_SOURCES += wallet/init.cpp
libdigibyte_node_a_CPPFLAGS += $(BDB_CPPFLAGS)
endif
if !ENABLE_WALLET
libdigibyte_node_a_SOURCES += dummywallet.cpp
endif
#

# zmq #
if ENABLE_ZMQ
libdigibyte_zmq_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(ZMQ_CFLAGS)
libdigibyte_zmq_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_zmq_a_SOURCES = \
  zmq/zmqabstractnotifier.cpp \
  zmq/zmqnotificationinterface.cpp \
  zmq/zmqpublishnotifier.cpp \
  zmq/zmqrpc.cpp \
  zmq/zmqutil.cpp
endif
#

# wallet #
libdigibyte_wallet_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(BOOST_CPPFLAGS) $(BDB_CPPFLAGS) $(SQLITE_CFLAGS)
libdigibyte_wallet_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_wallet_a_SOURCES = \
  wallet/coincontrol.cpp \
  wallet/context.cpp \
  wallet/crypter.cpp \
  wallet/db.cpp \
  wallet/digidollarwallet.cpp \
  wallet/ddcoincontrol.cpp \
  wallet/dump.cpp \
  wallet/external_signer_scriptpubkeyman.cpp \
  wallet/feebumper.cpp \
  wallet/fees.cpp \
  wallet/interfaces.cpp \
  wallet/load.cpp \
  wallet/receive.cpp \
  wallet/rpc/addresses.cpp \
  wallet/rpc/backup.cpp \
  wallet/rpc/coins.cpp \
  wallet/rpc/encrypt.cpp \
  wallet/rpc/spend.cpp \
  wallet/rpc/signmessage.cpp \
  wallet/rpc/transactions.cpp \
  wallet/rpc/util.cpp \
  wallet/rpc/wallet.cpp \
  wallet/scriptpubkeyman.cpp \
  wallet/spend.cpp \
  wallet/transaction.cpp \
  wallet/wallet.cpp \
  wallet/walletdb.cpp \
  wallet/walletutil.cpp \
  wallet/coinselection.cpp \
  $(DIGIBYTE_CORE_H)

if USE_SQLITE
libdigibyte_wallet_a_SOURCES += wallet/sqlite.cpp
endif
if USE_BDB
libdigibyte_wallet_a_SOURCES += wallet/bdb.cpp wallet/salvage.cpp
endif
#

# wallet tool #
libdigibyte_wallet_tool_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(BOOST_CPPFLAGS)
libdigibyte_wallet_tool_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_wallet_tool_a_SOURCES = \
  wallet/wallettool.cpp \
  $(DIGIBYTE_CORE_H)
#

# crypto #
crypto_libdigibyte_crypto_base_la_CPPFLAGS = $(AM_CPPFLAGS)

# Specify -static in both CXXFLAGS and LDFLAGS so libtool will only build a
# static version of this library. We don't need a dynamic version, and a dynamic
# version can't be used on windows anyway because the library doesn't currently
# export DLL symbols.
crypto_libdigibyte_crypto_base_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -static
crypto_libdigibyte_crypto_base_la_LDFLAGS = $(AM_LDFLAGS) -static

crypto_libdigibyte_crypto_base_la_SOURCES = \
  crypto/aes.cpp \
  crypto/aes.h \
  crypto/aes_helper.h \
  crypto/chacha_poly_aead.h \
  crypto/chacha20.h \
  crypto/chacha20.cpp \
  crypto/chacha20poly1305.h \
  crypto/chacha20poly1305.cpp \
  crypto/common.h \
  crypto/hkdf_sha256_32.cpp \
  crypto/hkdf_sha256_32.h \
  crypto/hmac_sha256.cpp \
  crypto/hmac_sha256.h \
  crypto/hmac_sha512.cpp \
  crypto/hmac_sha512.h \
  crypto/poly1305.h \
  crypto/poly1305.cpp \
  crypto/muhash.h \
  crypto/muhash.cpp \
  crypto/ripemd160.cpp \
  crypto/ripemd160.h \
  crypto/sha1.cpp \
  crypto/sha1.h \
  crypto/sha256.cpp \
  crypto/sha256.h \
  crypto/sha3.cpp \
  crypto/sha3.h \
  crypto/sha512.cpp \
  crypto/sha512.h \
  crypto/siphash.cpp \
  crypto/siphash.h

if USE_ASM
crypto_libdigibyte_crypto_base_la_SOURCES += crypto/sha256_sse4.cpp
endif

# See explanation for -static in crypto_libdigibyte_crypto_base_la's LDFLAGS and
# CXXFLAGS above
crypto_libdigibyte_crypto_sse41_la_LDFLAGS = $(AM_LDFLAGS) -static
crypto_libdigibyte_crypto_sse41_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -static
crypto_libdigibyte_crypto_sse41_la_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libdigibyte_crypto_sse41_la_CXXFLAGS += $(SSE41_CXXFLAGS)
crypto_libdigibyte_crypto_sse41_la_CPPFLAGS += -DENABLE_SSE41
crypto_libdigibyte_crypto_sse41_la_SOURCES = crypto/sha256_sse41.cpp

# See explanation for -static in crypto_libdigibyte_crypto_base_la's LDFLAGS and
# CXXFLAGS above
crypto_libdigibyte_crypto_avx2_la_LDFLAGS = $(AM_LDFLAGS) -static
crypto_libdigibyte_crypto_avx2_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -static
crypto_libdigibyte_crypto_avx2_la_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libdigibyte_crypto_avx2_la_CXXFLAGS += $(AVX2_CXXFLAGS)
crypto_libdigibyte_crypto_avx2_la_CPPFLAGS += -DENABLE_AVX2
crypto_libdigibyte_crypto_avx2_la_SOURCES = crypto/sha256_avx2.cpp

# See explanation for -static in crypto_libdigibyte_crypto_base_la's LDFLAGS and
# CXXFLAGS above
crypto_libdigibyte_crypto_x86_shani_la_LDFLAGS = $(AM_LDFLAGS) -static
crypto_libdigibyte_crypto_x86_shani_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -static
crypto_libdigibyte_crypto_x86_shani_la_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libdigibyte_crypto_x86_shani_la_CXXFLAGS += $(X86_SHANI_CXXFLAGS)
crypto_libdigibyte_crypto_x86_shani_la_CPPFLAGS += -DENABLE_X86_SHANI
crypto_libdigibyte_crypto_x86_shani_la_SOURCES = crypto/sha256_x86_shani.cpp

# See explanation for -static in crypto_libdigibyte_crypto_base_la's LDFLAGS and
# CXXFLAGS above
crypto_libdigibyte_crypto_arm_shani_la_LDFLAGS = $(AM_LDFLAGS) -static
crypto_libdigibyte_crypto_arm_shani_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -static
crypto_libdigibyte_crypto_arm_shani_la_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libdigibyte_crypto_arm_shani_la_CXXFLAGS += $(ARM_SHANI_CXXFLAGS)
crypto_libdigibyte_crypto_arm_shani_la_CPPFLAGS += -DENABLE_ARM_SHANI
crypto_libdigibyte_crypto_arm_shani_la_SOURCES = crypto/sha256_arm_shani.cpp
#

# consensus #
libdigibyte_consensus_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
libdigibyte_consensus_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_consensus_a_SOURCES = \
  arith_uint256.cpp \
  arith_uint256.h \
  consensus/amount.h \
  consensus/digidollar.cpp \
  consensus/digidollar.h \
  consensus/digidollar_transaction_validation.cpp \
  consensus/digidollar_transaction_validation.h \
  consensus/digidollar_tx.cpp \
  consensus/digidollar_tx.h \
  consensus/merkle.cpp \
  consensus/merkle.h \
  consensus/params.h \
  consensus/tx_check.cpp \
  consensus/validation.h \
  crypto/blake.cpp \
  crypto/bmw.cpp \
  crypto/cubehash.cpp \
  crypto/echo.cpp \
  crypto/groestl.cpp \
  crypto/jh.cpp \
  crypto/keccak.cpp \
  crypto/luffa.cpp \
  crypto/shavite.cpp \
  crypto/simd.cpp \
  crypto/skein.cpp \
  crypto/hashgroestl.h \
  crypto/hashodo.h \
  crypto/hashqubit.h \
  crypto/hashskein.h \
  crypto/odocrypt.cpp \
  crypto/odocrypt.h \
  crypto/KeccakP-800-reference.cpp \
  crypto/scrypt.cpp \
  crypto/scrypt-sse2.cpp \
  crypto/sph_blake.h \
  crypto/sph_bmw.h \
  crypto/sph_cubehash.h \
  crypto/sph_echo.h \
  crypto/sph_groestl.h \
  crypto/sph_jh.h \
  crypto/sph_keccak.h \
  crypto/sph_luffa.h \
  crypto/sph_shavite.h \
  crypto/sph_simd.h \
  crypto/sph_skein.h \
  crypto/sph_types.h \
  crypto/scrypt.h \
  hash.cpp \
  hash.h \
  prevector.h \
  primitives/block.cpp \
  primitives/block.h \
  primitives/transaction.cpp \
  primitives/transaction.h \
  pubkey.cpp \
  pubkey.h \
  script/digibyteconsensus.cpp \
  script/interpreter.cpp \
  script/interpreter.h \
  script/script.cpp \
  script/script.h \
  script/script_error.cpp \
  script/script_error.h \
  serialize.h \
  span.h \
  tinyformat.h \
  uint256.cpp \
  uint256.h \
  util/strencodings.cpp \
  util/strencodings.h \
  version.h
#

# common #
libdigibyte_common_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(BOOST_CPPFLAGS) $(LIBCURL_CFLAGS)
libdigibyte_common_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_common_a_SOURCES = \
  addresstype.cpp \
  base58.cpp \
  bech32.cpp \
  chainparams.cpp \
  coins.cpp \
  common/args.cpp \
  common/bloom.cpp \
  common/config.cpp \
  common/init.cpp \
  common/interfaces.cpp \
  common/run_command.cpp \
  common/settings.cpp \
  common/system.cpp \
  compressor.cpp \
  consensus/dca.cpp \
  consensus/dca.h \
  consensus/dd_mint_anchor.cpp \
  consensus/dd_mint_anchor.h \
  consensus/err.cpp \
  consensus/err.h \
  consensus/volatility.cpp \
  consensus/volatility.h \
  core_read.cpp \
  core_write.cpp \
  deploymentinfo.cpp \
  digidollar/digidollar.cpp \
  digidollar/scripts.cpp \
  digidollar/txbuilder.cpp \
  digidollar/validation.cpp \
  external_signer.cpp \
  init/common.cpp \
  kernel/chainparams.cpp \
  key.cpp \
  key_io.cpp \
  merkleblock.cpp \
  net_types.cpp \
  netaddress.cpp \
  netbase.cpp \
  net_permissions.cpp \
  oracle/bundle_manager.cpp \
  oracle/bundle_manager.h \
  oracle/exchange.cpp \
  oracle/exchange.h \
  oracle/mock_oracle.cpp \
  oracle/mock_oracle.h \
  oracle/musig2_aggregator.cpp \
  oracle/musig2_aggregator.h \
  oracle/musig2_orchestrator.cpp \
  oracle/musig2_orchestrator.h \
  oracle/musig2_oracle_participation.cpp \
  oracle/musig2_oracle_participation.h \
  oracle/musig2_session.cpp \
  oracle/musig2_session.h \
  oracle/musig2_session_manager.cpp \
  oracle/musig2_session_manager.h \
  oracle/musig2_messages.h \
  oracle/signing_orchestrator.cpp \
  oracle/signing_orchestrator.h \
  oracle/node.cpp \
  oracle/node.h \
  outputtype.cpp \
  policy/feerate.cpp \
  policy/policy.cpp \
  primitives/oracle.cpp \
  primitives/oracle.h \
  protocol.cpp \
  psbt.cpp \
  rpc/external_signer.cpp \
  rpc/rawtransaction_util.cpp \
  rpc/request.cpp \
  rpc/util.cpp \
  scheduler.cpp \
  script/descriptor.cpp \
  script/miniscript.cpp \
  script/sign.cpp \
  script/signingprovider.cpp \
  script/solver.cpp \
  warnings.cpp \
  $(DIGIBYTE_CORE_H)

if USE_LIBEVENT
libdigibyte_common_a_CPPFLAGS += $(EVENT_CFLAGS)
libdigibyte_common_a_SOURCES += common/url.cpp
endif
#

# util #
libdigibyte_util_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
libdigibyte_util_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_util_a_SOURCES = \
  support/lockedpool.cpp \
  chainparamsbase.cpp \
  clientversion.cpp \
  logging.cpp \
  random.cpp \
  randomenv.cpp \
  streams.cpp \
  support/cleanse.cpp \
  sync.cpp \
  util/asmap.cpp \
  util/batchpriority.cpp \
  util/bip32.cpp \
  util/bytevectorhash.cpp \
  util/chaintype.cpp \
  util/check.cpp \
  util/error.cpp \
  util/exception.cpp \
  util/fees.cpp \
  util/fs.cpp \
  util/fs_helpers.cpp \
  util/getuniquepath.cpp \
  util/hasher.cpp \
  util/sock.cpp \
  util/syserror.cpp \
  util/message.cpp \
  util/moneystr.cpp \
  util/rbf.cpp \
  util/readwritefile.cpp \
  util/signalinterrupt.cpp \
  util/thread.cpp \
  util/threadinterrupt.cpp \
  util/threadnames.cpp \
  util/serfloat.cpp \
  util/spanparsing.cpp \
  util/strencodings.cpp \
  util/string.cpp \
  util/time.cpp \
  util/tokenpipe.cpp \
  $(DIGIBYTE_CORE_H)
#

# cli #
libdigibyte_cli_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
libdigibyte_cli_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libdigibyte_cli_a_SOURCES = \
  compat/stdin.h \
  compat/stdin.cpp \
  rpc/client.cpp \
  $(DIGIBYTE_CORE_H)

nodist_libdigibyte_util_a_SOURCES = $(srcdir)/obj/build.h
#

# digibyted & digibyte-node binaries #
digibyte_daemon_sources = digibyted.cpp
digibyte_bin_cppflags = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
digibyte_bin_cxxflags = $(AM_CXXFLAGS) $(PIE_FLAGS)
digibyte_bin_ldflags = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) $(PTHREAD_FLAGS)

if TARGET_WINDOWS
digibyte_daemon_sources += digibyted-res.rc
endif

digibyte_bin_ldadd = \
  $(LIBDIGIBYTE_WALLET) \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBUNIVALUE) \
  $(LIBDIGIBYTE_ZMQ) \
  $(LIBDIGIBYTE_CONSENSUS) \
  $(LIBDIGIBYTE_CRYPTO) \
  $(LIBLEVELDB) \
  $(LIBMEMENV) \
  $(LIBSECP256K1)

digibyte_bin_ldadd += $(BDB_LIBS) $(MINIUPNPC_LIBS) $(NATPMP_LIBS) $(EVENT_PTHREADS_LIBS) $(EVENT_LIBS) $(ZMQ_LIBS) $(SQLITE_LIBS) $(LIBCURL_LIBS)

digibyted_SOURCES = $(digibyte_daemon_sources) init/digibyted.cpp
digibyted_CPPFLAGS = $(digibyte_bin_cppflags)
digibyted_CXXFLAGS = $(digibyte_bin_cxxflags)
digibyted_LDFLAGS = $(digibyte_bin_ldflags)
digibyted_LDADD = $(LIBDIGIBYTE_NODE) $(digibyte_bin_ldadd)

digibyte_node_SOURCES = $(digibyte_daemon_sources) init/digibyte-node.cpp
digibyte_node_CPPFLAGS = $(digibyte_bin_cppflags)
digibyte_node_CXXFLAGS = $(digibyte_bin_cxxflags)
digibyte_node_LDFLAGS = $(digibyte_bin_ldflags)
digibyte_node_LDADD = $(LIBDIGIBYTE_NODE) $(digibyte_bin_ldadd) $(LIBDIGIBYTE_IPC) $(LIBMULTIPROCESS_LIBS)

# digibyte-cli binary #
digibyte_cli_SOURCES = digibyte-cli.cpp
digibyte_cli_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(EVENT_CFLAGS)
digibyte_cli_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
digibyte_cli_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) $(PTHREAD_FLAGS)

if TARGET_WINDOWS
digibyte_cli_SOURCES += digibyte-cli-res.rc
endif

digibyte_cli_LDADD = \
  $(LIBDIGIBYTE_CLI) \
  $(LIBUNIVALUE) \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBDIGIBYTE_CRYPTO)

digibyte_cli_LDADD += $(EVENT_LIBS)
#

# digibyte-tx binary #
digibyte_tx_SOURCES = digibyte-tx.cpp
digibyte_tx_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
digibyte_tx_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
digibyte_tx_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) $(PTHREAD_FLAGS)

if TARGET_WINDOWS
digibyte_tx_SOURCES += digibyte-tx-res.rc
endif

digibyte_tx_LDADD = \
  $(LIBUNIVALUE) \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBDIGIBYTE_CONSENSUS) \
  $(LIBDIGIBYTE_CRYPTO) \
  $(LIBSECP256K1)
#

# digibyte-wallet binary #
digibyte_wallet_SOURCES = digibyte-wallet.cpp
digibyte_wallet_SOURCES += init/digibyte-wallet.cpp
digibyte_wallet_CPPFLAGS = $(digibyte_bin_cppflags)
digibyte_wallet_CXXFLAGS = $(digibyte_bin_cxxflags)
digibyte_wallet_LDFLAGS = $(digibyte_bin_ldflags)
digibyte_wallet_LDADD = \
  $(LIBDIGIBYTE_WALLET_TOOL) \
  $(LIBDIGIBYTE_WALLET) \
  $(LIBDIGIBYTE_NODE) \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBUNIVALUE) \
  $(LIBDIGIBYTE_CONSENSUS) \
  $(LIBDIGIBYTE_CRYPTO) \
  $(LIBLEVELDB) \
  $(LIBMEMENV) \
  $(LIBSECP256K1) \
  $(BDB_LIBS) \
  $(SQLITE_LIBS) \
  $(LIBCURL_LIBS)

if TARGET_WINDOWS
digibyte_wallet_SOURCES += digibyte-wallet-res.rc
endif
#

# digibyte-util binary #
digibyte_util_SOURCES = digibyte-util.cpp
digibyte_util_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
digibyte_util_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
digibyte_util_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) $(PTHREAD_FLAGS)

if TARGET_WINDOWS
digibyte_util_SOURCES += digibyte-util-res.rc
endif

digibyte_util_LDADD = \
  $(LIBDIGIBYTE_COMMON) \
  $(LIBDIGIBYTE_UTIL) \
  $(LIBUNIVALUE) \
  $(LIBDIGIBYTE_CONSENSUS) \
  $(LIBDIGIBYTE_CRYPTO) \
  $(LIBSECP256K1)
#

# digibyte-chainstate binary #
digibyte_chainstate_SOURCES = digibyte-chainstate.cpp
digibyte_chainstate_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES) $(BOOST_CPPFLAGS)
digibyte_chainstate_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)

digibyte_chainstate_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(PTHREAD_FLAGS) $(LIBTOOL_APP_LDFLAGS) -static
digibyte_chainstate_LDADD = $(LIBDIGIBYTEKERNEL)

# libtool is unable to calculate this indirect dependency, presumably because it's a subproject.
# libsecp256k1 only needs to be linked in when libdigibytekernel is static.
digibyte_chainstate_LDADD += $(LIBSECP256K1)
#

# digibytekernel library #
if BUILD_DIGIBYTE_KERNEL_LIB
lib_LTLIBRARIES += $(LIBDIGIBYTEKERNEL)

libdigibytekernel_la_LDFLAGS = $(AM_LDFLAGS) -no-undefined $(RELDFLAGS) $(PTHREAD_FLAGS)
libdigibytekernel_la_LIBADD = $(LIBDIGIBYTE_CRYPTO) $(LIBLEVELDB) $(LIBMEMENV) $(LIBSECP256K1)
libdigibytekernel_la_CPPFLAGS = $(AM_CPPFLAGS) -I$(builddir)/obj -I$(srcdir)/secp256k1/include -DBUILD_DIGIBYTE_INTERNAL $(BOOST_CPPFLAGS) $(LEVELDB_CPPFLAGS)

# libdigibytekernel requires default symbol visibility, explicitly specify that
# here so that things still work even when user configures with
#   --enable-reduce-exports
#
# Note this is a quick hack that will be removed as we incrementally define what
# to export from the library.
libdigibytekernel_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) -fvisibility=default

# TODO: libdigibytekernel is a work in progress consensus engine library, as more
#       and more modules are decoupled from the consensus engine, this list will
#       shrink to only those which are absolutely necessary.
libdigibytekernel_la_SOURCES = \
  kernel/digibytekernel.cpp \
  arith_uint256.cpp \
  chain.cpp \
  clientversion.cpp \
  coins.cpp \
  compressor.cpp \
  consensus/merkle.cpp \
  consensus/tx_check.cpp \
  consensus/tx_verify.cpp \
  core_read.cpp \
  dbwrapper.cpp \
  deploymentinfo.cpp \
  deploymentstatus.cpp \
  flatfile.cpp \
  hash.cpp \
  kernel/chain.cpp \
  kernel/checks.cpp \
  kernel/chainparams.cpp \
  kernel/coinstats.cpp \
  kernel/context.cpp \
  kernel/cs_main.cpp \
  kernel/mempool_persist.cpp \
  kernel/mempool_removal_reason.cpp \
  key.cpp \
  logging.cpp \
  node/blockstorage.cpp \
  node/chainstate.cpp \
  node/utxo_snapshot.cpp \
  policy/feerate.cpp \
  policy/fees.cpp \
  policy/packages.cpp \
  policy/policy.cpp \
  policy/rbf.cpp \
  policy/settings.cpp \
  pow.cpp \
  primitives/block.cpp \
  primitives/transaction.cpp \
  pubkey.cpp \
  random.cpp \
  randomenv.cpp \
  scheduler.cpp \
  script/interpreter.cpp \
  script/script.cpp \
  script/script_error.cpp \
  script/sigcache.cpp \
  script/solver.cpp \
  signet.cpp \
  streams.cpp \
  support/cleanse.cpp \
  support/lockedpool.cpp \
  sync.cpp \
  txdb.cpp \
  txmempool.cpp \
  uint256.cpp \
  util/batchpriority.cpp \
  util/chaintype.cpp \
  util/check.cpp \
  util/exception.cpp \
  util/fs.cpp \
  util/fs_helpers.cpp \
  util/getuniquepath.cpp \
  util/hasher.cpp \
  util/moneystr.cpp \
  util/rbf.cpp \
  util/serfloat.cpp \
  util/signalinterrupt.cpp \
  util/strencodings.cpp \
  util/string.cpp \
  util/syserror.cpp \
  util/thread.cpp \
  util/threadnames.cpp \
  util/time.cpp \
  util/tokenpipe.cpp \
  validation.cpp \
  validationinterface.cpp \
  versionbits.cpp \
  warnings.cpp

# Required for obj/build.h to be generated first.
# More details: https://www.gnu.org/software/automake/manual/html_node/Built-Sources-Example.html
libdigibytekernel_la-clientversion.l$(OBJEXT): obj/build.h
endif # BUILD_DIGIBYTE_KERNEL_LIB
#

# digibyteconsensus library #
if BUILD_DIGIBYTE_LIBS
lib_LTLIBRARIES += $(LIBDIGIBYTECONSENSUS)

include_HEADERS = script/digibyteconsensus.h
libdigibyteconsensus_la_SOURCES = support/cleanse.cpp $(crypto_libdigibyte_crypto_base_la_SOURCES) $(libdigibyte_consensus_a_SOURCES)

libdigibyteconsensus_la_LDFLAGS = $(AM_LDFLAGS) -no-undefined $(RELDFLAGS) -Wl,-undefined,dynamic_lookup
libdigibyteconsensus_la_LIBADD = $(LIBSECP256K1)
libdigibyteconsensus_la_CPPFLAGS = $(AM_CPPFLAGS) -I$(builddir)/obj -I$(srcdir)/secp256k1/include -DBUILD_DIGIBYTE_INTERNAL
libdigibyteconsensus_la_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)

endif
#

CTAES_DIST =  crypto/ctaes/bench.c
CTAES_DIST += crypto/ctaes/ctaes.c
CTAES_DIST += crypto/ctaes/ctaes.h
CTAES_DIST += crypto/ctaes/README.md
CTAES_DIST += crypto/ctaes/test.c

CLEANFILES = $(EXTRA_LIBRARIES)

CLEANFILES += *.gcda *.gcno
CLEANFILES += compat/*.gcda compat/*.gcno
CLEANFILES += consensus/*.gcda consensus/*.gcno
CLEANFILES += crc32c/src/*.gcda crc32c/src/*.gcno
CLEANFILES += crypto/*.gcda crypto/*.gcno
CLEANFILES += index/*.gcda index/*.gcno
CLEANFILES += interfaces/*.gcda interfaces/*.gcno
CLEANFILES += node/*.gcda node/*.gcno
CLEANFILES += policy/*.gcda policy/*.gcno
CLEANFILES += primitives/*.gcda primitives/*.gcno
CLEANFILES += rpc/*.gcda rpc/*.gcno
CLEANFILES += script/*.gcda script/*.gcno
CLEANFILES += support/*.gcda support/*.gcno
CLEANFILES += univalue/*.gcda univalue/*.gcno
CLEANFILES += util/*.gcda util/*.gcno
CLEANFILES += wallet/*.gcda wallet/*.gcno
CLEANFILES += wallet/test/*.gcda wallet/test/*.gcno
CLEANFILES += zmq/*.gcda zmq/*.gcno
CLEANFILES += obj/build.h

EXTRA_DIST = $(CTAES_DIST)


config/digibyte-config.h: config/stamp-h1
	@$(MAKE) -C $(top_builddir) $(subdir)/$(@)
config/stamp-h1: $(top_srcdir)/$(subdir)/config/digibyte-config.h.in $(top_builddir)/config.status
	$(AM_V_at)$(MAKE) -C $(top_builddir) $(subdir)/$(@)
$(top_srcdir)/$(subdir)/config/digibyte-config.h.in:  $(am__configure_deps)
	$(AM_V_at)$(MAKE) -C $(top_srcdir) $(subdir)/config/digibyte-config.h.in

clean-local:
	-$(MAKE) -C secp256k1 clean
	-rm -f leveldb/*/*.gcda leveldb/*/*.gcno leveldb/helpers/memenv/*.gcda leveldb/helpers/memenv/*.gcno
	-rm -f config.h
	-rm -rf test/__pycache__

.rc.o:
	@test -f $(WINDRES) || (echo "windres $(WINDRES) not found, but is required to compile windows resource files"; exit 1)
	## FIXME: How to get the appropriate modulename_CPPFLAGS in here?
	$(AM_V_GEN) $(WINDRES) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(CPPFLAGS) -DWINDRES_PREPROC -i $< -o $@

check-symbols: $(bin_PROGRAMS)
	@echo "Skipping symbol checks for DigiByte GUIX build"
	@true

check-security: $(bin_PROGRAMS)
if HARDEN
	@echo "Checking binary security..."
	$(AM_V_at) $(PYTHON) $(top_srcdir)/contrib/devtools/security-check.py $(bin_PROGRAMS)
endif

libdigibyte_ipc_mpgen_input = \
  ipc/capnp/echo.capnp \
  ipc/capnp/init.capnp
EXTRA_DIST += $(libdigibyte_ipc_mpgen_input)
%.capnp:

# Explicitly list dependencies on generated headers as described in
# https://www.gnu.org/software/automake/manual/html_node/Built-Sources-Example.html#Recording-Dependencies-manually
ipc/capnp/libdigibyte_ipc_a-protocol.$(OBJEXT): $(libdigibyte_ipc_mpgen_input:=.h)

if BUILD_MULTIPROCESS
LIBDIGIBYTE_IPC=libdigibyte_ipc.a
libdigibyte_ipc_a_SOURCES = \
  ipc/capnp/context.h \
  ipc/capnp/init-types.h \
  ipc/capnp/protocol.cpp \
  ipc/capnp/protocol.h \
  ipc/context.h \
  ipc/exception.h \
  ipc/interfaces.cpp \
  ipc/process.cpp \
  ipc/process.h \
  ipc/protocol.h
libdigibyte_ipc_a_CPPFLAGS = $(AM_CPPFLAGS) $(DIGIBYTE_INCLUDES)
libdigibyte_ipc_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS) $(LIBMULTIPROCESS_CFLAGS)

include $(MPGEN_PREFIX)/include/mpgen.mk
libdigibyte_ipc_mpgen_output = \
  $(libdigibyte_ipc_mpgen_input:=.c++) \
  $(libdigibyte_ipc_mpgen_input:=.h) \
  $(libdigibyte_ipc_mpgen_input:=.proxy-client.c++) \
  $(libdigibyte_ipc_mpgen_input:=.proxy-server.c++) \
  $(libdigibyte_ipc_mpgen_input:=.proxy-types.c++) \
  $(libdigibyte_ipc_mpgen_input:=.proxy-types.h) \
  $(libdigibyte_ipc_mpgen_input:=.proxy.h)
nodist_libdigibyte_ipc_a_SOURCES = $(libdigibyte_ipc_mpgen_output)
CLEANFILES += $(libdigibyte_ipc_mpgen_output)
endif

%.raw.h: %.raw
	@$(MKDIR_P) $(@D)
	$(AM_V_GEN) { \
	 echo "static unsigned const char $(*F)_raw[] = {" && \
	 $(HEXDUMP) -v -e '8/1 "0x%02x, "' -e '"\n"' $< | $(SED) -e 's/0x  ,//g' && \
	 echo "};"; \
	} > "$@.new" && mv -f "$@.new" "$@"

include Makefile.minisketch.include
include Makefile.crc32c.include
include Makefile.leveldb.include

include Makefile.test_util.include
include Makefile.test_fuzz.include

include Makefile.test.include

if ENABLE_BENCH
include Makefile.bench.include
endif

if ENABLE_QT
include Makefile.qt.include
endif

if ENABLE_QT_TESTS
include Makefile.qttest.include
endif

include Makefile.univalue.include
