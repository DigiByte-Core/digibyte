// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2020 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_POLICY_FEERATE_H
#define DIGIBYTE_POLICY_FEERATE_H

#include <amount.h>
#include <serialize.h>

#include <string>

const std::string CURRENCY_UNIT = "DGB"; // One formatted unit
const std::string CURRENCY_ATOM = "dbit"; // One indivisible minimum value unit

/* Used to determine type of fee estimation requested */
enum class FeeEstimateMode {
    UNSET,        //!< Use default settings based on other criteria
    ECONOMICAL,   //!< Force estimateSmartFee to use non-conservative estimates
    CONSERVATIVE, //!< Force estimateSmartFee to use conservative estimates
    DGB_KVB,      //!< Use DGB/kvB fee rate unit
    DBIT_VB,      //!< Use dbit/vB fee rate unit
};

/**
 * Fee rate in digibits per kilobyte: CAmount / kB
 */
class CFeeRate
{
private:
    CAmount nDigibitsPerK; // unit is digibits-per-1,000-bytes

public:
    /** Fee rate of 0 digibits per kB */
    CFeeRate() : nDigibitsPerK(0) { }
    template<typename I>
    explicit CFeeRate(const I _nDigibitsPerK): nDigibitsPerK(_nDigibitsPerK) {
        // We've previously had bugs creep in from silent double->int conversion...
        static_assert(std::is_integral<I>::value, "CFeeRate should be used without floats");
    }
    /** Constructor for a fee rate in digibits per kvB (dbit/kvB).
     *
     *  Passing a num_bytes value of COIN (1e8) returns a fee rate in digibits per vB (dbit/vB),
     *  e.g. (nFeePaid * 1e8 / 1e3) == (nFeePaid / 1e5),
     *  where 1e5 is the ratio to convert from DGB/kvB to dbit/vB.
     */
    CFeeRate(const CAmount& nFeePaid, uint32_t num_bytes);
    /**
     * Return the fee in digibits for the given size in bytes.
     * If the calculated fee would have fractional digibits, then the returned fee will always be rounded up to the nearest digibit.
     */
    CAmount GetFee(uint32_t num_bytes) const;
    /**
     * Return the fee in digibits for a size of 1000 bytes
     */
    CAmount GetFeePerK() const { return GetFee(1000); }
    friend bool operator<(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK < b.nDigibitsPerK; }
    friend bool operator>(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK > b.nDigibitsPerK; }
    friend bool operator==(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK == b.nDigibitsPerK; }
    friend bool operator<=(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK <= b.nDigibitsPerK; }
    friend bool operator>=(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK >= b.nDigibitsPerK; }
    friend bool operator!=(const CFeeRate& a, const CFeeRate& b) { return a.nDigibitsPerK != b.nDigibitsPerK; }
    CFeeRate& operator+=(const CFeeRate& a) { nDigibitsPerK += a.nDigibitsPerK; return *this; }
    std::string ToString(const FeeEstimateMode& fee_estimate_mode = FeeEstimateMode::DGB_KVB) const;

    SERIALIZE_METHODS(CFeeRate, obj) { READWRITE(obj.nDigibitsPerK); }
};

#endif //  DIGIBYTE_POLICY_FEERATE_H
