// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_DIGIDOLLARMINTCAPABILITY_H
#define DIGIBYTE_WALLET_DIGIDOLLARMINTCAPABILITY_H

#include <outputtype.h>
#include <util/translation.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

namespace wallet {

//! Check mint wallet support without reserving addresses or requiring an unlock.
//! An empty error permits construction; key derivation and signing still must succeed.
inline bilingual_str GetDigiDollarMintWalletError(const CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return _("Private keys are disabled for this wallet. Restore a backup with the matching private keys, or create and fund a new descriptor wallet with private keys to mint DigiDollar.");
    }
    if (!wallet.IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        return _("This legacy wallet cannot mint DigiDollar. Back it up, then create a descriptor wallet with private keys and transfer spendable DGB to it. Keep the original wallet and backup for existing funds.");
    }

    const auto* owner = wallet.GetScriptPubKeyMan(OutputType::BECH32M, /*internal=*/false);
    if (!wallet.IsHDEnabled() || !owner || !owner->CanGetAddresses() || !owner->HavePrivateKeys()) {
        return _("This wallet cannot generate DigiDollar owner keys. Restore a backup with the required private keys and receiving addresses, or create and fund a new descriptor wallet with private keys.");
    }
    const auto* change = wallet.GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/true);
    if (!change || !change->CanGetAddresses(/*internal=*/true) || !change->HavePrivateKeys()) {
        return _("This wallet cannot generate the change addresses needed for DigiDollar minting. Restore a complete wallet backup, or create and fund a new descriptor wallet with private keys.");
    }
    return {};
}

} // namespace wallet

#endif // DIGIBYTE_WALLET_DIGIDOLLARMINTCAPABILITY_H
