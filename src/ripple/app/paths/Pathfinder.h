//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_PATHFINDER_H
#define RIPPLE_PATHFINDER_H

namespace ripple {

/** Calculates payment paths.

    The @ref RippleCalc determines the quality of the found paths.

    @see RippleCalc
*/
class Pathfinder
{
public:
    /** Construct a pathfinder with an issuer.*/
    Pathfinder (
        RippleLineCache::ref cache,
        Account const& srcAccount,
        Account const& dstAccount,
        Currency const& uSrcCurrency,
        Account const& uSrcIssuer,
        STAmount const& dstAmount);

    /** Construct a pathfinder without an issuer.*/
    Pathfinder (
        RippleLineCache::ref cache,
        Account const& srcAccount,
        Account const& dstAccount,
        Currency const& uSrcCurrency,
        STAmount const& dstAmount);

    ~Pathfinder();

    static void initPathTable ();

    bool findPaths (int searchLevel);

    /** Compute the rankings of the paths. */
    void computePathRanks (int maxPaths);

    /* Get the best paths, up to maxPaths in number, from mCompletePaths.

       On return, if fullLiquidityPath is not empty, then it contains the best
       additional single path which can consume all the liquidity.
    */
    STPathSet getBestPaths (
        int maxPaths,
        STPath& fullLiquidityPath,
        STPathSet& extraPaths,
        Account const& srcIssuer);

    enum NodeType
    {
        nt_SOURCE,     // The source account: with an issuer account, if needed.
        nt_ACCOUNTS,   // Accounts that connect from this source/currency.
        nt_BOOKS,      // Order books that connect to this currency.
        nt_XRP_BOOK,   // The order book from this currency to XRP.
        nt_DEST_BOOK,  // The order book to the destination currency/issuer.
        nt_DESTINATION // The destination account only.
    };

    // The PathType is a list of the NodeTypes for a path.
    using PathType = std::vector <NodeType>;

    // PaymentType represents the types of the source and destination currencies
    // in a path request.
    enum PaymentType
    {
        pt_XRP_to_XRP,
        pt_XRP_to_nonXRP,
        pt_nonXRP_to_XRP,
        pt_nonXRP_to_same,   // Destination currency is the same as source.
        pt_nonXRP_to_nonXRP  // Destination currency is NOT the same as source.
    };

    struct PathRank
    {
        std::uint64_t quality;
        std::uint64_t length;
        STAmount liquidity;
        int index;
    };

private:
    /*
      Call graph of Pathfinder methods.

      findPaths:
          addPathsForType:
              addLinks:
                  addLink:
                      getPathsOut
                      issueMatchesOrigin
                      isNoRippleOut:
                          isNoRipple

      computePathRanks:
          rippleCalculate
          getPathLiquidity:
              rippleCalculate

      getBestPaths
     */


    // Add all paths of one type to mCompletePaths.
    STPathSet& addPathsForType (PathType const& type);

    bool issueMatchesOrigin (Issue const&);

    int getPathsOut (
        Currency const& currency,
        Account const& account,
        bool isDestCurrency,
        Account const& dest);

    void addLink (
        STPath const& currentPath,
        STPathSet& incompletePaths,
        int addFlags);

    // Call addLink() for each path in currentPaths.
    void addLinks (
        STPathSet const& currentPaths,
        STPathSet& incompletePaths,
        int addFlags);

    // Compute the liquidity for a path.  Return tesSUCCESS if it has has enough
    // liquidity to be worth keeping, otherwise an error.
    TER getPathLiquidity (
        STPath const& path,            // IN:  The path to check.
        STAmount const& minDstAmount,  // IN:  The minimum output this path must
                                       //      deliver to be worth keeping.
        STAmount& amountOut,           // OUT: The actual liquidity on the path.
        uint64_t& qualityOut) const;   // OUT: The returned initial quality

    // Does this path end on an account-to-account link whose last account has
    // set the "no ripple" flag on the link?
    bool isNoRippleOut (STPath const& currentPath);

    // Is the "no ripple" flag set from one account to another?
    bool isNoRipple (
        Account const& fromAccount,
        Account const& toAccount,
        Currency const& currency);

    void rankPaths (
        int maxPaths,
        STPathSet const& paths,
        std::vector <PathRank>& rankedPaths);

    Account mSrcAccount;
    Account mDstAccount;
    STAmount mDstAmount;
    Currency mSrcCurrency;
    boost::optional<Account> mSrcIssuer;
    STAmount mSrcAmount;
    /** The amount remaining from mSrcAccount after the default liquidity has
        been removed. */
    STAmount mRemainingAmount;

    Ledger::pointer mLedger;
    LoadEvent::pointer m_loadEvent;
    RippleLineCache::pointer mRLCache;

    STPathElement mSource;
    STPathSet mCompletePaths;
    std::vector<PathRank> mPathRanks;
    std::map<PathType, STPathSet> mPaths;

    hash_map<Issue, int> mPathsOutCountMap;

    // Add ripple paths
    static std::uint32_t const afADD_ACCOUNTS = 0x001;

    // Add order books
    static std::uint32_t const afADD_BOOKS = 0x002;

    // Add order book to XRP only
    static std::uint32_t const afOB_XRP = 0x010;

    // Must link to destination currency
    static std::uint32_t const afOB_LAST = 0x040;

    // Destination account only
    static std::uint32_t const afAC_LAST = 0x080;
};

} // ripple

#endif
