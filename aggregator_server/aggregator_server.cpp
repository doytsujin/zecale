// Copyright (c) 2015-2020 Clearmatics Technologies Ltd
//
// SPDX-License-Identifier: LGPL-3.0+

// Read the zecale config, include the appropriate pairing selector and define
// the corresponding pairing parameters type.

#include "libzecale/core/aggregator_circuit_wrapper.hpp"
#include "libzecale/core/application_pool.hpp"
#include "libzecale/serialization/proto_utils.hpp"
#include "zecale_config.h"

#include <api/aggregator.grpc.pb.h>
#include <boost/program_options.hpp>
#include <fstream>
#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <iostream>
#include <libsnark/common/data_structures/merkle_tree.hpp>
#include <libzeth/circuits/circuit_types.hpp>
#include <libzeth/core/utils.hpp>
#include <libzeth/serialization/proto_utils.hpp>
#include <libzeth/serialization/r1cs_serialization.hpp>
#include <libzeth/zeth_constants.hpp>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>

namespace proto = google::protobuf;
namespace po = boost::program_options;

// Set the wrapper curve type (wpp) based on the build configuration.
#if defined(ZECALE_CURVE_MNT6)
#include "libzecale/circuits/pairing/mnt_pairing_params.hpp"
using wpp = libff::mnt6_pp;
#elif defined(ZECALE_CURVE_BW6_761)
#include "libzecale/circuits/pairing/bw6_761_pairing_params.hpp"
using wpp = libff::bw6_761_pp;
#else
#error "ZECALE_CURVE_* variable not set to supported curve"
#endif

// The nested curve type (npp)
using npp = libzecale::other_curve<wpp>;

// Set both wrapper and nested snark schemes based on the build configuration.
#if defined(ZECALE_SNARK_PGHR13)
#include <libzecale/circuits/pghr13_verifier/pghr13_verifier_parameters.hpp>
#include <libzeth/snarks/pghr13/pghr13_api_handler.hpp>
using wverifier = libzecale::pghr13_verifier_parameters<wpp>;
using wapi_handler = libzeth::pghr13_api_handler<wpp>;
using nsnark = libzeth::pghr13_snark<npp>;
using napi_handler = libzeth::pghr13_api_handler<npp>;
#elif defined(ZECALE_SNARK_GROTH16)
#include <libzecale/circuits/groth16_verifier/groth16_verifier_parameters.hpp>
#include <libzeth/snarks/groth16/groth16_api_handler.hpp>
using wverifier = libzecale::groth16_verifier_parameters<wpp>;
using wapi_handler = libzeth::groth16_api_handler<wpp>;
using nsnark = libzeth::groth16_snark<npp>;
using napi_handler = libzeth::groth16_api_handler<npp>;
#else
#error "ZECALE_SNARK_* variable not set to supported ZK snark"
#endif

using wsnark = typename wverifier::snark;

static const size_t batch_size = 1;

/// The aggregator_server class inherits from the Aggregator service defined in
/// the proto files, and provides an implementation of the service.
class aggregator_server final : public zecale_proto::Aggregator::Service
{
private:
    libzecale::
        aggregator_circuit_wrapper<npp, wpp, nsnark, wverifier, batch_size>
            aggregator;

    // The keypair is the result of the setup for the aggregation circuit
    wsnark::keypair keypair;

    // The nested verification key is the vk used to verify the nested proofs
    std::map<std::string, libzecale::application_pool<npp, nsnark, batch_size>>
        pools_map;

public:
    explicit aggregator_server(
        libzecale::
            aggregator_circuit_wrapper<npp, wpp, nsnark, wverifier, batch_size>
                &aggregator,
        const wsnark::keypair &keypair)
        : aggregator(aggregator), keypair(keypair)
    {
        // Nothing
    }

    grpc::Status GetVerificationKey(
        grpc::ServerContext * /*context*/,
        const proto::Empty * /*request*/,
        zeth_proto::VerificationKey *response) override
    {
        std::cout << "[ACK] Received the request to get the verification key"
                  << std::endl;
        std::cout << "[DEBUG] Preparing verification key for response..."
                  << std::endl;
        try {
            wapi_handler::verification_key_to_proto(this->keypair.vk, response);
        } catch (const std::exception &e) {
            std::cout << "[ERROR] " << e.what() << std::endl;
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT, grpc::string(e.what()));
        } catch (...) {
            std::cout << "[ERROR] In catch all" << std::endl;
            return grpc::Status(grpc::StatusCode::UNKNOWN, "");
        }

        return grpc::Status::OK;
    }

    grpc::Status RegisterApplication(
        grpc::ServerContext * /*context*/,
        const zecale_proto::ApplicationRegistration *registration,
        proto::Empty * /*response*/) override
    {
        std::cout << "[ACK] Received 'register application' request"
                  << std::endl;
        std::cout << "[DEBUG] Registering application..." << std::endl;
        try {
            // Add the application to the list of supported applications on the
            // aggregator server.
            typename nsnark::verification_key registered_vk =
                napi_handler::verification_key_from_proto(registration->vk());
            libzecale::application_pool<npp, nsnark, batch_size> app_pool(
                registration->name(), registered_vk);
            this->pools_map[registration->name()] = app_pool;
        } catch (const std::exception &e) {
            std::cout << "[ERROR] " << e.what() << std::endl;
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT, grpc::string(e.what()));
        } catch (...) {
            std::cout << "[ERROR] In catch all" << std::endl;
            return grpc::Status(grpc::StatusCode::UNKNOWN, "");
        }

        return grpc::Status::OK;
    }

    grpc::Status GenerateAggregateProof(
        grpc::ServerContext * /*context*/,
        const zecale_proto::ApplicationName *app_name,
        zeth_proto::ExtendedProof *proof) override
    {
        std::cout
            << "[ACK] Received the request to generate an aggregation proof"
            << std::endl;
        try {
            std::cout << "[DEBUG] Pop batch from the pool..." << std::endl;
            // Select the application pool corresponding to the request
            libzecale::application_pool<npp, nsnark, batch_size> app_pool =
                this->pools_map[app_name->name()];
            // Retrieve batch from the pool
            std::array<
                libzecale::transaction_to_aggregate<npp, nsnark>,
                batch_size>
                batch = app_pool.get_next_batch();

            std::cout << "[DEBUG] Parse batch and generate witness..."
                      << std::endl;
            // Get batch of proofs to aggregate
            std::array<const libzeth::extended_proof<npp, nsnark> *, batch_size>
                extended_proofs{nullptr};
            for (size_t i = 0; i < batch.size(); i++) {
                extended_proofs[i] = &(batch[i].extended_proof());
            }

            // Retrieve the application verification key for the proof
            // aggregation
            nsnark::verification_key nested_vk = app_pool.verification_key();

            std::cout << "[DEBUG] Generating the proof..." << std::endl;
            libzeth::extended_proof<wpp, wsnark> wrapping_proof =
                this->aggregator.prove(
                    nested_vk, extended_proofs, this->keypair.pk);

            std::cout << "[DEBUG] Displaying the extended proof" << std::endl;
            wrapping_proof.write_json(std::cout);

            std::cout << "[DEBUG] Preparing response..." << std::endl;
            wapi_handler::extended_proof_to_proto(wrapping_proof, proof);
        } catch (const std::exception &e) {
            std::cout << "[ERROR] " << e.what() << std::endl;
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT, grpc::string(e.what()));
        } catch (...) {
            std::cout << "[ERROR] In catch all" << std::endl;
            return grpc::Status(grpc::StatusCode::UNKNOWN, "");
        }

        return grpc::Status::OK;
    }

    grpc::Status SubmitTransaction(
        grpc::ServerContext * /*context*/,
        const zecale_proto::TransactionToAggregate *transaction,
        proto::Empty * /*response*/) override
    {
        std::cout << "[ACK] Received the request to submit transaction"
                  << std::endl;
        std::cout << "[DEBUG] Submitting transaction..." << std::endl;
        try {
            // Add the application to the list of applications supported by the
            // server.
            libzecale::transaction_to_aggregate<npp, nsnark> tx = libzecale::
                transaction_to_aggregate_from_proto<npp, napi_handler>(
                    *transaction);
            libzecale::application_pool<npp, nsnark, batch_size> app_pool =
                this->pools_map[transaction->application_name()];
            app_pool.add_tx(tx);
        } catch (const std::exception &e) {
            std::cout << "[ERROR] " << e.what() << std::endl;
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT, grpc::string(e.what()));
        } catch (...) {
            std::cout << "[ERROR] In catch all" << std::endl;
            return grpc::Status(grpc::StatusCode::UNKNOWN, "");
        }

        return grpc::Status::OK;
    }
};

std::string get_server_version()
{
    char buffer[100];
    int n;
    // Defined in the zethConfig file
    n = snprintf(
        buffer,
        100,
        "Version %d.%d",
        ZECALE_VERSION_MAJOR,
        ZECALE_VERSION_MINOR);
    if (n < 0) {
        return "Version <Not specified>";
    }
    std::string version(buffer);
    return version;
}

void display_server_start_message()
{
    std::string copyright =
        "Copyright (c) 2015-2020 Clearmatics Technologies Ltd";
    std::string license = "SPDX-License-Identifier: LGPL-3.0+";
    std::string project = "R&D Department: PoC for a privacy preserving "
                          "scalability solution on Ethereum";
    std::string version = get_server_version();
    std::string warning = "**WARNING:** This code is a research-quality proof "
                          "of concept, DO NOT use in production!";

    std::cout << "\n=====================================================\n";
    std::cout << copyright << "\n";
    std::cout << license << "\n";
    std::cout << project << "\n";
    std::cout << version << "\n";
    std::cout << warning << "\n";
    std::cout << "=====================================================\n"
              << std::endl;
}

static void RunServer(
    libzecale::
        aggregator_circuit_wrapper<npp, wpp, nsnark, wverifier, batch_size>
            &aggregator,
    const typename wsnark::keypair &keypair)
{
    // Listen for incoming connections on 0.0.0.0:50052
    // TODO: Move this in a config file
    std::string server_address("0.0.0.0:50052");

    aggregator_server service(aggregator, keypair);

    grpc::ServerBuilder builder;

    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);

    // Finally assemble the server.
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "[DEBUG] Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    display_server_start_message();
    server->Wait();
}

#ifdef ZKSNARK_GROTH16
static wsnark::KeypairT load_keypair(const std::string &keypair_file)
{
    std::ifstream in(keypair_file, std::ios_base::in | std::ios_base::binary);
    in.exceptions(
        std::ios_base::eofbit | std::ios_base::badbit | std::ios_base::failbit);
    return wsnark::keypair_read_bytes(in);
}
#endif

int main(int argc, char **argv)
{
    // Options
    po::options_description options("");
    options.add_options()(
        "keypair,k", po::value<std::string>(), "file to load keypair from");
#ifdef DEBUG
    options.add_options()(
        "jr1cs,j",
        po::value<boost::filesystem::path>(),
        "file in which to export the r1cs in json format");
#endif

    auto usage = [&]() {
        std::cout << "Usage:"
                  << "\n"
                  << "  " << argv[0] << " [<options>]\n"
                  << "\n";
        std::cout << options;
        std::cout << std::endl;
    };

    std::string keypair_file;
#ifdef DEBUG
    boost::filesystem::path jr1cs_file;
#endif
    try {
        po::variables_map vm;
        po::store(
            po::command_line_parser(argc, argv).options(options).run(), vm);
        if (vm.count("help")) {
            usage();
            return 0;
        }
        if (vm.count("keypair")) {
            keypair_file = vm["keypair"].as<std::string>();
        }
#ifdef DEBUG
        if (vm.count("jr1cs")) {
            jr1cs_file = vm["jr1cs"].as<boost::filesystem::path>();
        }
#endif
    } catch (po::error &error) {
        std::cerr << " ERROR: " << error.what() << std::endl;
        usage();
        return 1;
    }

    // We inititalize the curve parameters here
    std::cout << "[INFO] Init params of both curves" << std::endl;
    npp::init_public_params();
    wpp::init_public_params();

    libzecale::
        aggregator_circuit_wrapper<npp, wpp, nsnark, wverifier, batch_size>
            aggregator;
    wsnark::keypair keypair = [&keypair_file, &aggregator]() {
        if (!keypair_file.empty()) {
#ifdef ZKSNARK_GROTH16
            std::cout << "[INFO] Loading keypair: " << keypair_file
                      << std::endl;
            return load_keypair(keypair_file);
#else
            std::cout << "Keypair loading not supported in this config"
                      << std::endl;
            exit(1);
#endif
        }

        std::cout << "[INFO] Generate new keypair" << std::endl;
        wsnark::keypair keypair = aggregator.generate_trusted_setup();
        return keypair;
    }();

#ifdef DEBUG
    // Run only if the flag is set
    if (!jr1cs_file.empty()) {
        std::cout << "[DEBUG] Dump R1CS to json file" << std::endl;
        std::ofstream jr1cs_stream(jr1cs_file.c_str());
        libzeth::r1cs_write_json<wpp>(
            aggregator.get_constraint_system(), jr1cs_stream);
    }
#endif

    std::cout << "[INFO] Setup successful, starting the server..." << std::endl;
    RunServer(aggregator, keypair);
    return 0;
}
