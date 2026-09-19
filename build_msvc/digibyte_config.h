// Copyright (c) 2018-2020 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DIGIBYTE_CONFIG_H
#define DIGIBYTE_DIGIBYTE_CONFIG_H

/* Version Build */
#define CLIENT_VERSION_BUILD 6

/* Version is release */
#define CLIENT_VERSION_IS_RELEASE true

/* Major version */
#define CLIENT_VERSION_MAJOR 9

/* Minor version */
#define CLIENT_VERSION_MINOR 26

/* Copyright holder(s) before %s replacement */
#define COPYRIGHT_HOLDERS "The %s developers"

/* Copyright holder(s) */
#define COPYRIGHT_HOLDERS_FINAL "The DigiByte Core developers"

/* Replacement for %s in copyright holders string */
#define COPYRIGHT_HOLDERS_SUBSTITUTION "DigiByte Core"

/* Copyright year */
#define COPYRIGHT_YEAR 2026

/* Define to 1 to enable wallet functions */
#define ENABLE_WALLET 1

/* Define to 1 to enable BDB wallet */
#define USE_BDB 1

/* Define to 1 to enable SQLite wallet */
#define USE_SQLITE 1

/* Define this symbol to enable ZMQ functions */
#define ENABLE_ZMQ 1

/* define if external signer support is enabled (requires Boost::Process) */
#define ENABLE_EXTERNAL_SIGNER /**/

/* Define to 1 if you have the declaration of `be16toh', and to 0 if you
   don't. */
#define HAVE_DECL_BE16TOH 0

/* Define to 1 if you have the declaration of `be32toh', and to 0 if you
   don't. */
#define HAVE_DECL_BE32TOH 0

/* Define to 1 if you have the declaration of `be64toh', and to 0 if you
   don't. */
#define HAVE_DECL_BE64TOH 0

/* Define to 1 if you have the declaration of `bswap_16', and to 0 if you
   don't. */
#define HAVE_DECL_BSWAP_16 0

/* Define to 1 if you have the declaration of `bswap_32', and to 0 if you
   don't. */
#define HAVE_DECL_BSWAP_32 0

/* Define to 1 if you have the declaration of `bswap_64', and to 0 if you
   don't. */
#define HAVE_DECL_BSWAP_64 0

/* Define to 1 if you have the declaration of `fork', and to 0 if you don't.
   */
#define HAVE_DECL_FORK 0

/* Define to 1 if you have the declaration of `htobe16', and to 0 if you
   don't. */
#define HAVE_DECL_HTOBE16 0

/* Define to 1 if you have the declaration of `htobe32', and to 0 if you
   don't. */
#define HAVE_DECL_HTOBE32 0

/* Define to 1 if you have the declaration of `htobe64', and to 0 if you
   don't. */
#define HAVE_DECL_HTOBE64 0

/* Define to 1 if you have the declaration of `htole16', and to 0 if you
   don't. */
#define HAVE_DECL_HTOLE16 0

/* Define to 1 if you have the declaration of `htole32', and to 0 if you
   don't. */
#define HAVE_DECL_HTOLE32 0

/* Define to 1 if you have the declaration of `htole64', and to 0 if you
   don't. */
#define HAVE_DECL_HTOLE64 0

/* Define to 1 if you have the declaration of `le16toh', and to 0 if you
   don't. */
#define HAVE_DECL_LE16TOH 0

/* Define to 1 if you have the declaration of `le32toh', and to 0 if you
   don't. */
#define HAVE_DECL_LE32TOH 0

/* Define to 1 if you have the declaration of `le64toh', and to 0 if you
   don't. */
#define HAVE_DECL_LE64TOH 0

/* Define to 1 if you have the declaration of `setsid', and to 0 if you don't.
   */
#define HAVE_DECL_SETSID 0

/* Define if the dllexport attribute is supported. */
#define HAVE_DLLEXPORT_ATTRIBUTE 1

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "https://github.com/digibyte/digibyte/issues"

/* Define to the full name of this package. */
#define PACKAGE_NAME "DigiByte Core"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "DigiByte Core 9.26.6"

/* Define to the home page for this package. */
#define PACKAGE_URL "https://digibytecore.org/"

/* Define to the version of this package. */
#define PACKAGE_VERSION "9.26.6"

/* Define this symbol if the minimal qt platform exists */
#define QT_QPA_PLATFORM_MINIMAL 1

/* Define this symbol if the qt platform is windows */
#define QT_QPA_PLATFORM_WINDOWS 1

/* Define this symbol if qt plugins are static */
#define QT_STATICPLUGIN 1

/* Define if QR support should be compiled in */
#define USE_QRCODE 1

/* Windows Universal Platform constraints */
#if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP)
/* Either a desktop application without API restrictions, or and older system
   before these macros were defined. */

/* ::wsystem is available */
#define HAVE_SYSTEM 1

#endif // !WINAPI_FAMILY || WINAPI_FAMILY_DESKTOP_APP

#endif //DIGIBYTE_DIGIBYTE_CONFIG_H
