// Copyright (c) 2015-2020 Clearmatics Technologies Ltd
//
// SPDX-License-Identifier: LGPL-3.0+

#ifndef __ZECALE_CORE_APPLICATION_POOL_TCC__
#define __ZECALE_CORE_APPLICATION_POOL_TCC__

#include <array>
#include <libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/r1cs_ppzksnark.hpp>
#include <libzeth/core/extended_proof.hpp>
#include <queue>

namespace libzecale
{

template<typename nppT, typename nsnarkT, size_t NumProofs>
application_pool<nppT, nsnarkT, NumProofs>::application_pool(
    const std::string &name, typename nsnarkT::verification_key vk)
    : _name(name), _tx_pool()
{
    this->_verification_key =
        std::make_shared<typename nsnarkT::verification_key>(vk);
}

template<typename nppT, typename nsnarkT, size_t NumProofs>
std::array<transaction_to_aggregate<nppT, nsnarkT>, NumProofs> application_pool<
    nppT,
    nsnarkT,
    NumProofs>::get_next_batch()
{
    std::array<transaction_to_aggregate<nppT, nsnarkT>, NumProofs> batch;
    if (this->_tx_pool.size() < NumProofs) {
        for (size_t i = 0; i < this->_tx_pool.size(); i++) {
            batch[i] = this->_tx_pool.top();
            _tx_pool.pop();
        }
        return batch;
    }

    for (size_t i = 0; i < NumProofs; i++) {
        batch[i] = this->_tx_pool.top();
        _tx_pool.pop();
    }
    return batch;
}

} // namespace libzecale

#endif // __ZECALE_CORE_APPLICATION_POOL_TCC__
