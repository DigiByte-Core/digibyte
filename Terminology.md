| Title       | Terminology                 |
|-------------|-----------------------------|
| Description |                             |
| Published   | true                        |
| Date        | 2023-10-22T10:20:55.378Z    |
| Tags        |                             |
| Editor      | markdown                    |
| DateCreated | 2023-10-22T10:19:41.696Z    |


# Purpose

Every day we participate in a society where acronyms, abbreviations and slang expressions are used widely to describe certain objects or subjects.

These terminologies can be frightening for the uninitiated, and makes participating in conversations a lot harder than it needs to be.

In this article we break it down for you, to help you get a grasp of some of the commonly used terminology in the DigiByte ecosystem.

# Technology

General terminology relating to Blockchain technology.

## Blockchain

A blockchain is a type of database that distributes and shares data between nodes on a network.

On a traditional database, data are usually stored in tables as separate entries for each data point. On a blockchain the data is grouped together as a collection in what we call blocks.

On a cryptocurrency blockchain, each data entry is a transaction. For a transaction to be broadcasted to the network, it first must go to what is called a mempool

Each block has a specific storage capacity written into the code base. On the DigiByte Protocol the storage capacity of each block is 1 MB (or one million bytes).  
When a block has reached its storage capacity, transactions will have to keep queuing in the [**mempool**](#mempool).

When a block is generated and broadcasted to the network, a [**hash**](#hash) is generated. When it's time for a new block to be generated, it uses the hash from the previous block combined with the new data in the current block to create a new unique hash, the process is the same for all new blocks.

This data structure is what creates a irreversible timeline with inherently immutable data. If you attempted to go back to a previous block and change the data, subsequently the hash would also change, and all blocks after that would have to be re-validated. The difficulty to perform such an action increases with every given block.

The notion of data being comprised as blocks and continuously broadcasted is what gives blockchain its name, a chain of blocks.

## Mempool

The memory pool, or _mempool,_ is a sort of temporary database, and is the first stop for a new transaction, where they sit in queue awaiting a miner to verify it.

Once a transaction is verified, otherwise known as “mined”, the transaction leaves the mempool and gets broadcasted onto the network in the currently comprising block.

On average, the DigiByte [**Blockchain**](#blockchain) verifies a block every 15 seconds. That means that when a block has reached its storage capacity, transactions must sit and wait in the _mempool_ for the next available block.

It should be noted that there are no guarantees that a transaction will make it into the next block, regardless of how long it's been waiting in queue in the mempool.
This is because transactions with the highest fee is prioritized and therefore processed first, as this is of benefit to the miner in terms of rewards received for the computational power they provide. This is the reason for why you may some times see increases in transaction fee on the network, because miners prioritize the highest reward.

In addition, there is a minimum transaction fee on the network, and transaction below this requirement might, in theory, never make it into a block and as a result not get broadcasted on the network.

## Hash

Hashing is a mathematical function that converts input of any length into an encrypted output of a fixed length, producing the _Hash_.
Hash functions are one-way, so once a hash is generated it can not be used to reverse-engineer the data it was generated from. Regardless of how many times you put the data through a hashing function, the resulting hash will always stay the same, this makes for the perfect way to verify sensitive data without the risk of exposing the content of it.

Knowing the hash for a set of data means you can quickly and easily compare data sets to verify that nothing has been altered or corrupted.

## UTXO

An _unspent transaction output_ (**UTXO**) is the technical terminology for the amount of cryptocurrency, in this specific case, DigiByte, remains after a transaction has taken place.

Each time you perform a transaction, a check is made to see how much of a currency is left unspent.  
You can view the UTXO as the change that is returned to you after you have paid for something with a larger bill than the price.

Each transaction requires both what is known as an [**Input**](#transaction-input) and the [**Output**](#transaction-output). This is what is commonly known as double-entry accounting. Don't worry if this term confused you, we explain it below.

### Transaction Input

The input needs to be (at least) enough to cover the charge (cost) entered by the user, this can be comprised of 1 or several UTXO's, depending on their balance and the cost.

This is a method called non-exact, meaning the input is not the exact same amount as the required output.

A transaction input is always the entirety of one or several UTXO's.

### Transaction Output

The transaction output is the selected amount of DigiBytes that is transacted.

The transaction output is exact.

This means an output is the exact required amount of DigiBytes that is going to be sent with the outgoing transaction and will not require the total funds available from the comprised UTXO's.

### **Example**

> This example assumes that Bill's wallet only contains funds from a total of two transactions, i.e UTXO's.
{.is-info}



Let's say that Bill wants to buy some Falafel, and he has 1000 DigiBytes in his account wallet, divided equally from two separate incoming transactions (500 DigiBytes each).

After Bill has selected all of his toppings and his favorite sauce, the Falafel will cost him 800 DigiBytes.

Bill opens up his mobile wallet, scans the vendor's DigiByte payment address, and enters 800 DGB as the amount to send.

Now the wallet will start comprising UTXO's as inputs for the transaction, in this case it will use both of the aforementioned UTXO's á 500 DGB each, so now the total input is exactly 1000 DGB.

But that's too much for the 800 DGB Falafel Bill wants to purchase.

This is where the Output comes in. An output of the required funds is created, corresponding to the 800 DigiBytes Bill entered into their transaction.

Now as the transaction goes out to the Falafel merchant, it will have an output of 800 DigiBytes, and the 200 remaining DigiBytes is send back to Bill as an _unspent transaction output._

Now the next time Bill wants to perform a transaction, there will be one UTXO with a balance of 200 DigiBytes ready to be used as an input for a new transaction.



### Key Points

-   A UTXO is the remaining amount of DigiBytes (cryptocurrency) remaining after a completed transaction
-   UTXO's are processes as part of both the beginning and end of a transaction.
-   After completion of a transaction, any unspent transaction outputs are recorded as available inputs for new transactions.

## PoW

_Proof of Work_ (PoW) is best described as a system where significant amount of effort is required in order to deter malicious use of computing power. Such as denial of service attacks, spam attacks, or otherwise of the sort.

It is a decentralized consensus model, and is today used to, among other things, to secure cryptocurrency transactions, including DigiByte.

With a chain such as DigiByte, the _proof of work_ is also generally referred to as “mining”. When miners work on a [**blockchain**](#blockchain) they compete to be the first miner to generate a valid hash for the data contained within a block. The hash, consisting of a string of numbers and letters, will always stay the same length given the data that is hashed (the [block header](#block-header)), but the hash string itself can differ.

For any computer to generate a valid hash would be very simple, this is why to provide proof of work, the chain applies a [**difficulty adjustment**](#difficulty-adjustment), resulting in a [**target hash**](#target-hash).

The lower the target hash, the fewer possible valid hashes, and the harder to generate one.

## Difficulty Adjustment

Within the code base of DigiByte, there is a fixed value of 15, representing the expected block time, the interval of which blocks are generated and broadcasted on the network. In reality we can never know how much time it takes for a miner to validate, also known as to “solve”, a block. This is where the difficulty adjustment comes in.

DigiByte is rather unique in this aspect, as it applies a real-time _difficulty adjustment_ with the use of the DigiByte developers' own technology known as [DigiShield](#digishield), meaning that it adjusts the difficulty for each block, on every algorithm.

In essence, the point of the difficulty adjustment is to ensure that the actual block time meets the expected block time, in this case 15 seconds.

The equation is pretty simple: 

`new_*difficulty = old_difficulty * (15 seconds) / (the time in seconds to mine the previous block)*`

***_#The above equation is an assumption, and should be fact-checked._***

## Target Hash

The _target hash_, in terms of DigiByte mining, is a numeric value that the hashed [**block header**](#block-header) must be either less than or equal to in order for the block to be considered valid and result in a reward for the miner.

## Block Header

The _block header_ contains three sets of block metadata. It is a 80 byte long string, and is comprised of:

-   4 byte long version number
-   32 byte long previous block [**hash**](#hash)
-   32 byte long [**Merkle root**](#merkle-root)
-   4 byte long block timestamp
-   4 byte long difficulty target
-   4 byte long [**nonce**](#nonce)

The _block header_ is used to identify a specific block on the [**blockchain**](#blockchain)**.** Essentially, it's a cryptographic hash of the data contained within the block, and is created by hashing the metadata mentioned above, twice.

## Merkle Root

The Merkle root is a [**hash**](#hash) of all the transaction hashes contained within a block. It is used to quickly verify the data, to ensure that it is complete and has not been altered or corrupted in any way.

## Nonce

The _Nonce,_ or “number used only once”, is a four-bit number added to the hashed block that when rehashed, meets the [**difficulty**](#difficulty-adjustment) restriction. The nonce is used to attempt to create a [**hash**](#hash) that is less or equal to the [**block header**](#block-header) hash of the block they are validating.

## Hash Power

TO BE WRITTEN

## ASIC Mining

TO BE WRITTEN

# Protocol Implementations

Terminology related to DigiByte Blockchain technology and implementations.

## DigiShield

#### Overview
**Activation Date:** February 2014.

_DigiShield_ is a hard fork implemented to protect the DigiByte blockchain from a multi-pool vulnerability. This vulnerability allowed mining pools to mine large amounts of DigiByte at low difficulty levels.

#### Purpose
- **Difficulty Adjustment:** DigiShield adjusts the mining difficulty between every block, instead of every 2016 blocks as originally designed.
- **Fair Mining Distribution:** It ensures fair mining distribution by preventing an unfair amount of DigiByte from being mined at low difficulty and stabilizing the network when large amounts of hash power leave.

#### Technical Details
- **Protection Against Chain Freeze:** By adjusting difficulty more frequently, DigiShield prevents potential temporary freezes of the blockchain, which can occur if the difficulty remains too high relative to the available hash power.
- **Reduction in Unfair Mining:** After implementation, there was a reported 50-80% decrease in unfairly mined coins.
- **Multi-Algorithm Support:** Initially, DigiByte used one algorithm, but with the addition of four more algorithms, DigiShield has evolved into MultiShield to provide protection across all algorithms.

#### Historical Context
- **Multi-Pool Attacks:** At the time of DigiShield’s implementation, multi-pool attacks were common on many Proof of Work chains, prompting the rapid development and deployment of this solution.

## MultiAlgo

#### Overview
_MultiAlgo_ is a term used to describe a hard fork in the DigiByte Core Protocol that incorporates multiple Proof of Work (PoW) mining methods. This approach allows for mining through various types of hardware.

#### Initial Implementation
DigiByte was initially launched with a single mining algorithm, **Scrypt**, which was compatible with Central Processing Units (CPUs).

#### Expansion to MultiAlgo
With the introduction of MultiAlgo, DigiByte expanded its mining capabilities to include Graphical Processing Units (GPUs) and ASIC miners. To support this expansion, four additional mining algorithms were integrated into the DigiByte Core Protocol:
- **SHA-256D**
- **Qubit**
- **Skein**
- **Groestl**

#### Current Usage
DigiByte continues to utilize a five-algorithm approach. However, on July 2, 2019, the **Groestl** algorithm was replaced with **OdoCrypt** to mitigate ASIC miner dominance and centralization.

## MultiShield

#### Overview
_MultiShield_ is a critical network upgrade within the DigiByte blockchain ecosystem.

#### Background
- Originally, DigiShield was designed for single-algorithm blockchains. However, DigiByte's multi-algorithm approach required a more sophisticated solution.
- MultiShield was introduced on December 10, 2014, at block height 400,000.

#### Real-Time Difficulty Adjustment
- MultiShield dynamically adjusts mining difficulty across all five DigiByte algorithms.
- This ensures consistent block timings and protects against sudden hash-power fluctuations.

#### Defense Against Attacks
- MultiShield guards against 51% attacks, where an attacker controls the majority of hash power.
- By increasing difficulty for the algorithm that finds a block and decreasing it for others, it prevents single-algorithm takeovers.
- Even if an attacker controlled significant hash power on one algorithm, MultiShield thwarts their efforts.

#### Impervious to Malicious Hash Power
- If an ASIC manufacturer inserted a backdoor into miners, MultiShield would prevent that hash rate from affecting block production.
- Unlike single-algorithm chains, DigiByte remains resilient.

#### Rented Attacks and Global Hash Power
- Rented attacks are common but ineffective against MultiShield.
- DigiByte's global hash power dominance, especially in Qubit, Myr-Gr, Skein, and Odocrypt, reinforces its security.

#### Seamless Upgrades
- MultiShield was the third successful network upgrade executed by DigiByte.

**Summary**
MultiShield enhances DigiByte's security, stability, and resistance to attacks, making it a robust and forward-thinking blockchain platform.

## DigiSpeed

#### Overview
_DigiSpeed_ is a pivotal network upgrade within the DigiByte ecosystem that significantly enhanced the blockchain's performance.

#### Background
- DigiByte initially boasted a fast block-timing of 60 seconds (by UTXO standards).
- Inspired by research from Microsoft Research Labs and ETH Zurich, the development team aimed to further improve efficiency.

#### The Upgrade
- On December 4, 2015, at block height 1,430,000, the DigiSpeed upgrade was implemented.
- Result: Block timing was halved from 30 to 15 seconds, making DigiByte the fastest Proof-of-Work blockchain.

#### Transaction Capacity Boost
- DigiSpeed introduced a forward-thinking approach: block-size doubling every two years.
- Purpose: Consistently increase transaction processing capacity.
- Goal: Maintain scalability and avoid limitations due to insufficient block size.

#### SegWit Integration
- The block-size doubling aspect was grandfathered into the Segregated Witness (SegWit) upgrade.
- SegWit provided a 4X weighted capacity increase.
- Expectations: Further refinement and reimplementation in the future.

**Summary**
DigiSpeed propelled DigiByte's speed, scalability, and adaptability, positioning it as a cutting-edge blockchain platform.

## Segregated Witness (SegWit)

#### Overview
**_Segregated Witness (SegWit)_** was activated on the DigiByte blockchain in April 2017. DigiByte was the first major blockchain to implement SegWit, preceding Litecoin by several weeks and Bitcoin by several months.

#### Purpose
- **Data Optimization:** SegWit optimizes data storage by segregating part of the transaction data.
- **Transaction Malleability:** It resolves transaction malleability issues, enhancing the security and reliability of transactions.

#### Technical Details
- **Increased Block Capacity:** By segregating the signatures from the transaction data, SegWit effectively increases the block capacity by approximately 4 times.
- **Atomic Swaps:** SegWit facilitates safe Atomic Swaps between DigiByte and various other blockchains.
- **N-Version Bits Upgrade:** The N-Version Bits upgrade was included alongside the SegWit activation.

## Digi-ID

**_Digi-ID_** is a fast and secure authentication method designed to replace standard usernames, passwords, two-factor timed one-time passwords (TOTP), building swipe-cards, and more. It addresses many vulnerabilities associated with traditional security methods, such as SIM-swapping, insecure SMS authentication, and phishing for rolling TOTP codes.

#### Applications
Digi-ID can be used for both traditional and non-traditional authentication purposes, including:
- Games
- Phone/PC applications
- Building security

It eliminates the need for multiple accounts by authorizing access to different aspects of a website, building, product, or platform with a single Digi-ID. This means users do not need separate logins for personal and company use.

#### Anonymity and Compliance
Digi-ID ensures anonymity by not storing or transmitting any information via the blockchain. No personally identifiable information is kept within the DigiByte mobile applications, and there is no data-logging. This design facilitates easy compliance with GDPR regulations.

#### Implementation
Digi-ID has been integrated into the Coinomi application suite and several other authenticator applications. This broad implementation lowers barriers to entry for new users and enhances audience penetration with existing software.

## DigiAssets

**_DigiAssets_** is a platform developed by DigiByte that enables the creation and management of digital assets on the blockchain. These assets can be either fungible or non-fungible and include a wide range of items such as:
- Tokens
- Credits
- Shares
- Event tickets
- Coins
- Trading cards
- Property deeds or titles
- Travel tickets

#### Verification and Transparency
DigiAssets are verified on the DigiByte blockchain, ensuring they are immutable, unforgeable, and transparent in terms of supply and ownership. This verification allows users to trade, send, or receive DigiAssets globally with ease.

#### Capabilities
DigiAssets offer standard features expected by users, including:
- The ability to burn asset supply
- Adding additional metadata to assets using key-value pairs

#### Platform Strengths
With DigiByte’s on-chain scalability, robust security, and strong decentralization, DigiAssets provides an ideal platform for launching any type of digital asset.

## Dandelion++

**_Dandelion++_** was introduced as the default transaction transmission method in the DigiByte 7.17.2 protocol upgrade. It is fully backwards-compatible with non-Dandelion supporting nodes. Due to the Odocrypt upgrade, all Core Wallets now support Dandelion.

#### Functionality
Dandelion++ works by initially distributing a transaction to a single connected node, rather than broadcasting it to all connected nodes as in the traditional mempool dispersion method. The individual node then has a 10% chance (determined by a virtual coin flip) to send the transaction to another single node. This process continues until the transaction is eventually broadcasted to the network, resembling the flowering of a dandelion.

#### Privacy Protection
Dandelion++ enhances user privacy by masking the source IP address from any snooping nodes. This makes it difficult for observers to determine the origin of a transaction with any certainty.

#### Impact on Transaction Speed
While Dandelion++ does not affect DigiByte's 15-second block timing, transactions may take slightly longer to reach the mempool. For transactions requiring additional speed, Dandelion++ can be disabled on a per-transaction basis.

## Odocrypt

**_Odocrypt_** is a unique mining algorithm used exclusively by the DigiByte blockchain. This exclusivity means there is no hash-power available for rent from third-party services, reinforcing DigiByte's dominance and enhancing network security.

#### Accessibility and Encouragement for Home Users
The hardware required for Odocrypt mining is not prohibitively expensive, unlike costly ASICs. This affordability encourages home users to participate in mining by downloading the blockchain through the DigiByte Core application. 

#### Documentation and Software
Odocrypt was launched with comprehensive documentation and software, enabling users to mine both solo and in pools. This focus on hobby mining helps distribute the blockchain more widely and contributes to the overall security of the DigiByte network.
