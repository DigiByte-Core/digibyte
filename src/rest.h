// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_REST_H
#define DIGIBYTE_REST_H

#include <rpc/protocol.h>
#include <span.h>

#include <string>

class CBlock;
class CBlockIndex;
enum class TxVerbosity;
namespace node { class BlockManager; }

enum class RESTResponseFormat {
    UNDEF,
    BINARY,
    HEX,
    JSON,
};

/** Prepare the complete response before the HTTP request is answered. */
struct RESTResponse {
    HTTPStatusCode status;
    std::string content_type;
    std::string body;
};

RESTResponse BuildRESTHeadersResponse(Span<const CBlockIndex* const> headers, const CBlockIndex* tip, RESTResponseFormat format);
RESTResponse BuildRESTBlockResponse(node::BlockManager& blockman, const CBlock& block, const CBlockIndex* tip,
                                    const CBlockIndex* index, RESTResponseFormat format, TxVerbosity verbosity);

/**
 * Parse a URI to get the data format and URI without data format
 * and query string.
 *
 * @param[out]  param   The strReq without the data format string and
 *                      without the query string (if any).
 * @param[in]   strReq  The URI to be parsed.
 * @return      RESTResponseFormat that was parsed from the URI.
 */
RESTResponseFormat ParseDataFormat(std::string& param, const std::string& strReq);

#endif // DIGIBYTE_REST_H
